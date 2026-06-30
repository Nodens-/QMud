/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_LuaCallbackEngine.cpp
 * Role: Unit coverage for Lua callback-engine dispatch, catalog, and callback-context semantics.
 */

#include "ColorPacking.h"
#include "LuaCallbackEngine.h"
#include "LuaExecutor.h"
#include "LuaExecutorWorker.h"
#include "LuaSupport.h"
#include "NativePluginRegistry.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "helpers/LuaExecutionUtils.h"
#include "helpers/PluginPathUtils.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QScopeGuard>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QTest>

#include <atomic>
#include <clocale>
#include <limits>
#include <memory>

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace
{
	class tst_LuaCallbackEngine final : public QObject
	{
			Q_OBJECT

		private slots:
			// NOLINTBEGIN(readability-convert-member-functions-to-static)
			void initTestCase();
			void directCallbackShapesRoundTrip();
			void wildcardAndStyleCallbackReceivesContextTables();
			void mxpCallbacksMarshalArguments();
			void modalYieldResumePreservesNumberAndStringCallback();
			void modalYieldResumePreservesStringInOutCallback();
			void modalYieldResumePreservesNoArgsCallback();
			void modalYieldResumePreservesBytesInOutCallback();
			void modalYieldResumeSupportsStackedModalCalls();
			void workerModalResumeDefersPostModalRuntimeMutations();
			void modalYieldCancelPreventsCallbackContinuation();
			void callbackCatalogObserverTracksFunctionPresence();
			void packageRestrictionsAreAppliedToExistingState();
			void worldLuaFileApisAcceptMixedSeparators();
			void worldLuaFileApisUseRuntimeHomeAcrossThreadAffinity();
			void worldLuaFileApisIgnoreProcessQmudHome();
			void luaVisiblePathApisReturnRelativePosix();
			void utilsMultiListBoxAcceptsMushclientArgumentOrder();
			void deferredRuntimeMutationBatchesPreserveOrderAndOwnership();
			void directExecutorDispatchesRealEngines();
			void callPluginMarshallingUsesTargetEngineState();
			void noArgsDispatchReportsCallbackFailure();
			void workerDispatchesPluginLifecycleCallbacksOnRealEngines();
			void workerCallbackBatchCapturesOutputMiniWindowAndSaveStateMutations();
			void workerColourOutputMatchesMushclientGroupingAndNewlineSemantics();
			void workerColourOutputPreservesIndexedNoteColour();
			void normalColourDefaultsMatchMushclientAcrossRuntimeAndCallbackPaths();
			void emptyColourTellDoesNotMutateCallbackOutputCache();
			void colourTellIgnoresTrailingLuaGsubReturnAndKeepsFollowingNote();
			void executeScriptNoteUsesRuntimeNoteColour();
			void selfPluginInfoMetadataFallsThroughToRuntime();
			void emptyPluginVariableIdReadsWorldVariables();
			void deleteVariableInvalidatesCallbackVariableSnapshot();
			void nativeShimDiscoveryIsAvailableWithoutShadowPlugin();
			void nativeLuaAudioSharedRuntimeStateCoversDirectAndCallPlugin();
			void blacklistedPluginsAreHiddenFromPluginApis();
			void triggerAnchoredColourOutputKeepsNativePromptText();
			void stringsAndWildcardsDispatchSuppliesSnapshotForCallbackReads();
			void callbackSnapshotSuppliesGetInfoAndMiniWindowReads();
			void miniWindowDragReleaseSeesResizedCallbackState();
			void deferredRuntimeMutationSkipsDestroyedRuntime();
			// NOLINTEND(readability-convert-member-functions-to-static)
	};

	void setEngineScript(LuaCallbackEngine &engine, const QString &script)
	{
		engine.setPluginInfo(QStringLiteral("Plugin.Id"), QStringLiteral("Plugin Name"),
		                     QStringLiteral("/tmp/plugin"));
		engine.setScriptText(script);
		QVERIFY(engine.loadScript());
	}

	bool luaGlobalBoolean(lua_State *state, const char *name)
	{
		lua_getglobal(state, name);
		const bool value = lua_toboolean(state, -1) != 0;
		lua_pop(state, 1);
		return value;
	}

	QString luaGlobalString(lua_State *state, const char *name)
	{
		lua_getglobal(state, name);
		const QString value = QString::fromUtf8(lua_tostring(state, -1));
		lua_pop(state, 1);
		return value;
	}

	QColor colorFromPackedValue(const long value)
	{
		const auto packed = static_cast<QMudColorRef>(value);
		return {qmudRed(packed), qmudGreen(packed), qmudBlue(packed)};
	}

	int safeQSizeToInt(const qsizetype size)
	{
		if (size <= 0)
			return 0;
		constexpr qsizetype kMaxInt = std::numeric_limits<int>::max();
		return size > kMaxInt ? std::numeric_limits<int>::max() : static_cast<int>(size);
	}

	template <typename Entries> QStringList logicalOutputLinesFromEntries(const Entries &entries)
	{
		QStringList lines;
		QString     currentLine;
		for (const WorldRuntime::LineEntry &entry : entries)
		{
			if ((entry.flags & WorldRuntime::LineHidden) != 0)
				continue;
			currentLine += entry.text;
			if (entry.hardReturn)
			{
				lines.push_back(currentLine);
				currentLine.clear();
			}
		}
		if (!currentLine.isEmpty())
			lines.push_back(currentLine);
		return lines;
	}

	QString acceptedModalStringResult(const QString &value)
	{
		QJsonObject object;
		object.insert(QStringLiteral("accepted"), true);
		object.insert(QStringLiteral("value"), value);
		return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
	}

	QSharedPointer<const LuaCallbackMiniWindowSnapshot>
	captureVariableDispatchSnapshotForTest(const WorldRuntime &runtime)
	{
		auto snapshot                       = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
		snapshot->worldVariablesSnapshot    = runtime.variableSnapshot();
		snapshot->hasWorldVariablesSnapshot = true;
		return snapshot;
	}

	QSharedPointer<const LuaCallbackMiniWindowSnapshot>
	captureRuntimeCounterDispatchSnapshotForTest(const WorldRuntime &runtime)
	{
		auto snapshot                        = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
		snapshot->hasRuntimeCountersSnapshot = true;
		snapshot->runtimeCounterValues.insert(QStringLiteral("notesInRgb"), runtime.notesInRgb());
		snapshot->runtimeCounterValues.insert(QStringLiteral("noteTextColour"), runtime.noteTextColour());
		snapshot->runtimeCounterValues.insert(QStringLiteral("noteColourFore"),
		                                      QVariant::fromValue<qlonglong>(runtime.noteColourFore()));
		snapshot->runtimeCounterValues.insert(QStringLiteral("noteColourBack"),
		                                      QVariant::fromValue<qlonglong>(runtime.noteColourBack()));
		snapshot->runtimeCounterValues.insert(QStringLiteral("noteStyle"), runtime.noteStyle());
		for (int index = 1; index <= 8; ++index)
		{
			const QColor boldColour = runtime.ansiColour(true, index);
			snapshot->boldAnsiColoursByIndex.insert(
			    index, boldColour.isValid() ? static_cast<long>(qmudRgb(boldColour.red(), boldColour.green(),
			                                                            boldColour.blue()))
			                                : 0);
			snapshot->normalAnsiColoursByIndex.insert(index, runtime.normalColour(index));
		}
		for (int index = 1; index <= MAX_CUSTOM; ++index)
		{
			snapshot->customTextColoursByIndex.insert(index, runtime.customColourText(index));
			snapshot->customBackgroundColoursByIndex.insert(index, runtime.customColourBackground(index));
		}
		return snapshot;
	}

	struct LuaStateDeleterForTest
	{
			/**
			 * @brief Closes Lua state owned by a test helper.
			 * @param state Lua state pointer.
			 */
			void operator()(lua_State *state) const
			{
				if (state)
					lua_close(state);
			}
	};

	using LuaStatePtr = std::unique_ptr<lua_State, LuaStateDeleterForTest>;

	LuaStatePtr makeLuaState()
	{
		LuaStatePtr state(luaL_newstate());
		luaL_openlibs(state.get());
		QMudLuaSupport::applyLua51Compat(state.get());
		return state;
	}

	void dispatchWorkerAndWait(const LuaExecutorWorker &executor, const LuaBatchDispatchRequest &request,
	                           LuaBatchDispatchResult &result)
	{
		QObject          completionTarget;
		std::atomic_bool completed{false};
		executor.dispatchBatchAsync(request, &completionTarget,
		                            [&](const LuaBatchDispatchResult &dispatchResult)
		                            {
			                            result = dispatchResult;
			                            completed.store(true, std::memory_order_release);
		                            });
		QTRY_VERIFY_WITH_TIMEOUT(completed.load(std::memory_order_acquire), 3000);
	}

	void dispatchWorkerAndWait(const LuaExecutorWorker &executor, const LuaBatchDispatchRequest &request)
	{
		LuaBatchDispatchResult unusedResult;
		dispatchWorkerAndWait(executor, request, unusedResult);
	}

	void initializeWorkerEngine(const LuaExecutorWorker                 &executor,
	                            const QSharedPointer<LuaCallbackEngine> &engine, const QString &script,
	                            WorldRuntime *runtime = nullptr)
	{
		QVERIFY(engine);
		LuaEngineObservedInitializationRequest initRequest;
		initRequest.engine          = engine.data();
		initRequest.runtime         = runtime;
		initRequest.scriptText      = script;
		initRequest.pluginId        = QStringLiteral("Plugin.Id");
		initRequest.pluginName      = QStringLiteral("Plugin Name");
		initRequest.pluginDirectory = QStringLiteral("/tmp/plugin");

		auto initRequests = QSharedPointer<QVector<LuaEngineObservedInitializationRequest>>::create();
		initRequests->push_back(std::move(initRequest));

		LuaBatchDispatchRequest request;
		request.kind            = LuaBatchDispatchKind::InitializeEnginesWithObservedCallbacksMany;
		request.initRequestsArg = initRequests;
		dispatchWorkerAndWait(executor, request);
	}

	void teardownWorkerEngine(const LuaExecutorWorker                 &executor,
	                          const QSharedPointer<LuaCallbackEngine> &engine)
	{
		QVERIFY(engine);
		LuaBatchDispatchRequest request;
		request.kind    = LuaBatchDispatchKind::TeardownEnginesMany;
		request.engines = {engine};
		dispatchWorkerAndWait(executor, request);
	}

	void executeDeferredMutations(LuaBatchDispatchResult &result)
	{
		for (LuaDeferredRuntimeMutationBatch &batch : result.deferredRuntimeMutationBatches)
		{
			for (std::function<void()> &mutation : batch.mutations)
				mutation();
		}
	}
} // namespace

// NOLINTBEGIN(readability-convert-member-functions-to-static)
void tst_LuaCallbackEngine::initTestCase()
{
	std::setlocale(LC_NUMERIC, "C");
}

void tst_LuaCallbackEngine::directCallbackShapesRoundTrip()
{
	LuaCallbackEngine engine;
	setEngineScript(engine, QStringLiteral(R"lua(
seen = {}
function cb_no_args()
  return true
end
function cb_string(value)
  seen.string = value
  return value == "abc"
end
function cb_bytes(value)
  seen.bytes = value
  return value == "bin"
end
function cb_bytes_inout(value)
  return value .. ":out"
end
function cb_string_inout(value)
  return value .. ":out"
end
function cb_num_string(number, value)
  seen.num_string = tostring(number) .. ":" .. value
  return number == 7 and value == "arg"
end
function cb_num_strings(number, one, two, three)
  seen.num_strings = tostring(number) .. ":" .. one .. two .. three
  return number == 4 and one .. two .. three == "abc"
end
function cb_two_nums(one, two, value)
  seen.two_nums = tostring(one) .. ":" .. tostring(two) .. ":" .. value
  return one == 2 and two == 3 and value == "go"
end
function cb_num_bytes(number, value)
  seen.num_bytes = tostring(number) .. ":" .. value
  return number == 9 and value == "bytes"
end
function cb_proc(value)
  seen.proc = value
end
)lua"));

	bool hasFunction = false;
	QVERIFY(engine.callFunctionNoArgs(QStringLiteral("cb_no_args"), &hasFunction, false));
	QVERIFY(hasFunction);
	QVERIFY(engine.callFunctionWithString(QStringLiteral("cb_string"), QStringLiteral("abc"), &hasFunction,
	                                      false));
	QVERIFY(hasFunction);
	QVERIFY(engine.callFunctionWithBytes(QStringLiteral("cb_bytes"), QByteArray("bin"), &hasFunction, false));
	QVERIFY(hasFunction);

	QByteArray bytes = QByteArray("bytes");
	QVERIFY(engine.callFunctionWithBytesInOut(QStringLiteral("cb_bytes_inout"), bytes, &hasFunction));
	QVERIFY(hasFunction);
	QCOMPARE(bytes, QByteArray("bytes:out"));

	QString text = QStringLiteral("text");
	QVERIFY(engine.callFunctionWithStringInOut(QStringLiteral("cb_string_inout"), text, &hasFunction));
	QVERIFY(hasFunction);
	QCOMPARE(text, QStringLiteral("text:out"));

	QVERIFY(engine.callFunctionWithNumberAndString(QStringLiteral("cb_num_string"), 7, QStringLiteral("arg"),
	                                               &hasFunction, false));
	QVERIFY(hasFunction);
	QVERIFY(engine.callFunctionWithNumberAndStrings(QStringLiteral("cb_num_strings"), 4, QStringLiteral("a"),
	                                                QStringLiteral("b"), QStringLiteral("c"), &hasFunction,
	                                                false));
	QVERIFY(hasFunction);
	QVERIFY(engine.callFunctionWithTwoNumbersAndString(QStringLiteral("cb_two_nums"), 2, 3,
	                                                   QStringLiteral("go"), &hasFunction, false));
	QVERIFY(hasFunction);
	QVERIFY(engine.callFunctionWithNumberAndBytes(QStringLiteral("cb_num_bytes"), 9, QByteArray("bytes"),
	                                              &hasFunction, false));
	QVERIFY(hasFunction);
	QVERIFY(
	    engine.callProcedureWithString(QStringLiteral("cb_proc"), QStringLiteral("procedure"), &hasFunction));
	QVERIFY(hasFunction);

	QVERIFY(engine.executeScript(QStringLiteral(R"lua(
assert(seen.string == "abc")
assert(seen.bytes == "bin")
assert(seen.num_string == "7:arg")
assert(seen.num_strings == "4:abc")
assert(seen.two_nums == "2:3:go")
assert(seen.num_bytes == "9:bytes")
assert(seen.proc == "procedure")
)lua"),
	                             QStringLiteral("verify direct callback shapes")));
	QCOMPARE(lua_gettop(engine.luaState()), 0);
}

void tst_LuaCallbackEngine::wildcardAndStyleCallbackReceivesContextTables()
{
	LuaCallbackEngine engine;
	setEngineScript(engine, QStringLiteral(R"lua(
function trigger_cb(first, second, wildcards, styles)
  trigger_seen = first .. "|" .. second .. "|" .. wildcards[0] .. "|" ..
                 wildcards.named .. "|" .. styles[1].text .. "|" ..
                 tostring(styles[2].style)
end
)lua"));

	QVector<LuaStyleRun> styleRuns;
	styleRuns.push_back({QStringLiteral("room"), 10, 11, 1});
	styleRuns.push_back({QStringLiteral(" exits"), 12, 13, 4});
	const QStringList            args{QStringLiteral("line"), QStringLiteral("match")};
	const QStringList            wildcards{QStringLiteral("whole"), QStringLiteral("capture")};
	const QMap<QString, QString> namedWildcards{
	    {QStringLiteral("named"), QStringLiteral("value")}
    };

	bool hasFunction = false;
	QVERIFY(engine.callFunctionWithStringsAndWildcards(QStringLiteral("trigger_cb"), args, wildcards,
	                                                   namedWildcards, &styleRuns, nullptr, &hasFunction, 6,
	                                                   true, 12, 345));
	QVERIFY(hasFunction);
	QCOMPARE(luaGlobalString(engine.luaState(), "trigger_seen"),
	         QStringLiteral("line|match|whole|value|room|4.0"));

	QVERIFY(engine.executeScript(QStringLiteral(R"lua(
assert(TriggerStyleRuns == nil)
)lua"),
	                             QStringLiteral("style table remains callback-local")));
}

void tst_LuaCallbackEngine::mxpCallbacksMarshalArguments()
{
	LuaCallbackEngine engine;
	setEngineScript(engine, QStringLiteral(R"lua(
mxp_seen = {}
function mxp_error(level, number, line, message)
  mxp_seen.error = tostring(level) .. ":" .. tostring(number) .. ":" ..
                   tostring(line) .. ":" .. message
  return false
end
function mxp_start()
  mxp_seen.start = true
end
function mxp_shutdown()
  mxp_seen.shutdown = true
end
function mxp_start_tag(name, args, attrs)
  mxp_seen.start_tag = name .. ":" .. args .. ":" .. attrs.href
  return true
end
function mxp_end_tag(name, text)
  mxp_seen.end_tag = name .. ":" .. text
end
function mxp_set_variable(name, contents)
  mxp_seen.variable = name .. ":" .. contents
end
)lua"));

	QVERIFY(!engine.callMxpError(QStringLiteral("mxp_error"), 2, 42, 7, QStringLiteral("bad tag")));
	engine.callMxpStartUp(QStringLiteral("mxp_start"));
	engine.callMxpShutDown(QStringLiteral("mxp_shutdown"));
	QVERIFY(engine.callMxpStartTag(QStringLiteral("mxp_start_tag"), QStringLiteral("send"),
	                               QStringLiteral("href='look'"),
	                               {
	                                   {QStringLiteral("href"), QStringLiteral("look")}
    }));
	engine.callMxpEndTag(QStringLiteral("mxp_end_tag"), QStringLiteral("send"), QStringLiteral("Look"));
	engine.callMxpSetVariable(QStringLiteral("mxp_set_variable"), QStringLiteral("room"),
	                          QStringLiteral("Dock"));

	QVERIFY(engine.executeScript(QStringLiteral(R"lua(
assert(mxp_seen.error == "2:42:7:bad tag")
assert(mxp_seen.start == true)
assert(mxp_seen.shutdown == true)
assert(mxp_seen.start_tag == "send:href='look':look")
assert(mxp_seen.end_tag == "send:Look")
assert(mxp_seen.variable == "room:Dock")
)lua"),
	                             QStringLiteral("verify mxp callbacks")));
}

void tst_LuaCallbackEngine::modalYieldResumePreservesNumberAndStringCallback()
{
	WorldRuntime runtime;
	auto         engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
colour_seen = false
function OnHotspot(flags, hotspot)
  modal_number_phase = "before"
  local colour = PickColour(-1)
  colour_seen = flags == 2 and hotspot == "tab" and colour == 255
  modal_number_phase = "after"
  return colour_seen
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines       = {engine};
	request.kind          = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.functionName  = QStringLiteral("OnHotspot");
	request.numberArg1    = 2;
	request.stringArg2    = QStringLiteral("tab");
	request.defaultResult = false;

	LuaBatchDispatchResult initialResult = executor.dispatchBatch(request);
	QVERIFY(initialResult.suspended);
	QVERIFY(initialResult.modalResumeId != 0);
	QCOMPARE(initialResult.suspendedEngineIndex, 0);
	QVERIFY(initialResult.hasPendingModalStringRequest);
	QVERIFY(initialResult.pendingModalStringRequest.guiCallable);
	QVERIFY(initialResult.pendingModalStringRequest.resultCallback);
	executeDeferredMutations(initialResult);
	QCOMPARE(luaGlobalString(engine->luaState(), "modal_number_phase"), QStringLiteral("before"));
	QVERIFY(!luaGlobalBoolean(engine->luaState(), "colour_seen"));

	LuaBatchDispatchRequest resumeRequest;
	resumeRequest.engines       = {engine};
	resumeRequest.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	resumeRequest.modalResumeId = initialResult.modalResumeId;
	resumeRequest.stringArg     = QStringLiteral("255");

	LuaBatchDispatchResult resumedResult = executor.dispatchBatch(resumeRequest);
	QVERIFY(!resumedResult.suspended);
	QVERIFY(resumedResult.boolResultValid);
	QVERIFY(resumedResult.boolResult);
	QVERIFY(resumedResult.hasFunctionValid);
	QVERIFY(resumedResult.hasFunction);
	executeDeferredMutations(resumedResult);
	QCOMPARE(luaGlobalString(engine->luaState(), "modal_number_phase"), QStringLiteral("after"));
	QVERIFY(luaGlobalBoolean(engine->luaState(), "colour_seen"));
}

void tst_LuaCallbackEngine::modalYieldResumePreservesStringInOutCallback()
{
	WorldRuntime runtime;
	auto         engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
string_seen = ""
function Transform(value)
  local choice = utils.inputbox("choose", "title", "")
  string_seen = value .. ":" .. choice
  return string_seen
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("Transform");
	request.stringArg    = QStringLiteral("before");

	LuaBatchDispatchResult initialResult = executor.dispatchBatch(request);
	QVERIFY(initialResult.suspended);
	QVERIFY(initialResult.modalResumeId != 0);
	QCOMPARE(initialResult.suspendedEngineIndex, 0);
	QVERIFY(initialResult.hasPendingModalStringRequest);
	QCOMPARE(initialResult.stringResult, QStringLiteral("before"));
	QCOMPARE(luaGlobalString(engine->luaState(), "string_seen"), QString());

	LuaBatchDispatchRequest resumeRequest;
	resumeRequest.engines       = {engine};
	resumeRequest.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	resumeRequest.modalResumeId = initialResult.modalResumeId;
	resumeRequest.stringArg     = acceptedModalStringResult(QStringLiteral("after"));

	LuaBatchDispatchResult resumedResult = executor.dispatchBatch(resumeRequest);
	QVERIFY(!resumedResult.suspended);
	QVERIFY(resumedResult.boolResultValid);
	QVERIFY(resumedResult.boolResult);
	QVERIFY(resumedResult.hasFunctionValid);
	QVERIFY(resumedResult.hasFunction);
	QCOMPARE(resumedResult.stringResult, QStringLiteral("before:after"));
	QCOMPARE(luaGlobalString(engine->luaState(), "string_seen"), QStringLiteral("before:after"));
}

void tst_LuaCallbackEngine::modalYieldResumePreservesNoArgsCallback()
{
	WorldRuntime runtime;
	auto         engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
noargs_seen = false
function OnPluginEnable()
  modal_noargs_phase = "before"
  local choice = utils.inputbox("choose", "title", "")
  noargs_seen = choice == "accepted"
  modal_noargs_phase = "after"
  return noargs_seen
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines       = {engine};
	request.kind          = LuaBatchDispatchKind::NoArgs;
	request.functionName  = QStringLiteral("OnPluginEnable");
	request.defaultResult = false;

	LuaBatchDispatchResult initialResult = executor.dispatchBatch(request);
	QVERIFY(initialResult.suspended);
	QVERIFY(initialResult.modalResumeId != 0);
	QCOMPARE(initialResult.suspendedEngineIndex, 0);
	QVERIFY(initialResult.hasPendingModalStringRequest);
	QVERIFY(!initialResult.boolResultValid);
	QVERIFY(!initialResult.hasFunctionValid);
	executeDeferredMutations(initialResult);
	QCOMPARE(luaGlobalString(engine->luaState(), "modal_noargs_phase"), QStringLiteral("before"));
	QVERIFY(!luaGlobalBoolean(engine->luaState(), "noargs_seen"));

	LuaBatchDispatchRequest resumeRequest;
	resumeRequest.engines       = {engine};
	resumeRequest.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	resumeRequest.modalResumeId = initialResult.modalResumeId;
	resumeRequest.stringArg     = acceptedModalStringResult(QStringLiteral("accepted"));

	LuaBatchDispatchResult resumedResult = executor.dispatchBatch(resumeRequest);
	QVERIFY(!resumedResult.suspended);
	QVERIFY(resumedResult.boolResultValid);
	QVERIFY(resumedResult.boolResult);
	QVERIFY(resumedResult.hasFunctionValid);
	QVERIFY(resumedResult.hasFunction);
	executeDeferredMutations(resumedResult);
	QCOMPARE(luaGlobalString(engine->luaState(), "modal_noargs_phase"), QStringLiteral("after"));
	QVERIFY(luaGlobalBoolean(engine->luaState(), "noargs_seen"));
}

void tst_LuaCallbackEngine::modalYieldResumePreservesBytesInOutCallback()
{
	WorldRuntime runtime;
	auto         engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
bytes_seen = false
function TransformBytes(value)
  modal_bytes_phase = "before"
  local suffix = utils.inputbox("choose", "title", "")
  bytes_seen = value == "payload" and suffix == "done"
  modal_bytes_phase = "after"
  return value .. ":" .. suffix
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::BytesInOut;
	request.functionName = QStringLiteral("TransformBytes");
	request.bytesArg     = QByteArray("payload");

	LuaBatchDispatchResult initialResult = executor.dispatchBatch(request);
	QVERIFY(initialResult.suspended);
	QVERIFY(initialResult.modalResumeId != 0);
	QCOMPARE(initialResult.suspendedEngineIndex, 0);
	QVERIFY(initialResult.hasPendingModalStringRequest);
	QCOMPARE(initialResult.bytesResult, QByteArray("payload"));
	executeDeferredMutations(initialResult);
	QCOMPARE(luaGlobalString(engine->luaState(), "modal_bytes_phase"), QStringLiteral("before"));
	QVERIFY(!luaGlobalBoolean(engine->luaState(), "bytes_seen"));

	LuaBatchDispatchRequest resumeRequest;
	resumeRequest.engines       = {engine};
	resumeRequest.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	resumeRequest.modalResumeId = initialResult.modalResumeId;
	resumeRequest.stringArg     = acceptedModalStringResult(QStringLiteral("done"));

	LuaBatchDispatchResult resumedResult = executor.dispatchBatch(resumeRequest);
	QVERIFY(!resumedResult.suspended);
	QVERIFY(resumedResult.boolResultValid);
	QVERIFY(resumedResult.boolResult);
	QVERIFY(resumedResult.hasFunctionValid);
	QVERIFY(resumedResult.hasFunction);
	QCOMPARE(resumedResult.bytesResult, QByteArray("payload:done"));
	executeDeferredMutations(resumedResult);
	QCOMPARE(luaGlobalString(engine->luaState(), "modal_bytes_phase"), QStringLiteral("after"));
	QVERIFY(luaGlobalBoolean(engine->luaState(), "bytes_seen"));
}

void tst_LuaCallbackEngine::modalYieldResumeSupportsStackedModalCalls()
{
	WorldRuntime runtime;
	auto         engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
stacked_seen = false
function OnHotspot(flags, hotspot)
  modal_stacked_phase = "before_first"
  local first = PickColour(-1)
  modal_stacked_phase = "before_second"
  local second = PickColour(-1)
  modal_stacked_phase = "after_second"
  stacked_seen = flags == 4 and hotspot == "stacked" and first == 10 and second == 20
  return stacked_seen
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines       = {engine};
	request.kind          = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.functionName  = QStringLiteral("OnHotspot");
	request.numberArg1    = 4;
	request.stringArg2    = QStringLiteral("stacked");
	request.defaultResult = false;

	LuaBatchDispatchResult firstSuspend = executor.dispatchBatch(request);
	QVERIFY(firstSuspend.suspended);
	QVERIFY(firstSuspend.modalResumeId != 0);
	QVERIFY(firstSuspend.hasPendingModalStringRequest);
	executeDeferredMutations(firstSuspend);
	QCOMPARE(luaGlobalString(engine->luaState(), "modal_stacked_phase"), QStringLiteral("before_first"));

	LuaBatchDispatchRequest firstResume;
	firstResume.engines       = {engine};
	firstResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	firstResume.modalResumeId = firstSuspend.modalResumeId;
	firstResume.stringArg     = QStringLiteral("10");

	LuaBatchDispatchResult secondSuspend = executor.dispatchBatch(firstResume);
	QVERIFY(secondSuspend.suspended);
	QVERIFY(secondSuspend.modalResumeId != 0);
	QVERIFY(secondSuspend.modalResumeId != firstSuspend.modalResumeId);
	QCOMPARE(secondSuspend.suspendedEngineIndex, 0);
	QVERIFY(secondSuspend.hasPendingModalStringRequest);
	executeDeferredMutations(secondSuspend);
	QCOMPARE(luaGlobalString(engine->luaState(), "modal_stacked_phase"), QStringLiteral("before_second"));
	QVERIFY(!luaGlobalBoolean(engine->luaState(), "stacked_seen"));

	LuaBatchDispatchRequest secondResume;
	secondResume.engines       = {engine};
	secondResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	secondResume.modalResumeId = secondSuspend.modalResumeId;
	secondResume.stringArg     = QStringLiteral("20");

	LuaBatchDispatchResult completed = executor.dispatchBatch(secondResume);
	QVERIFY(!completed.suspended);
	QVERIFY(completed.boolResultValid);
	QVERIFY(completed.boolResult);
	executeDeferredMutations(completed);
	QCOMPARE(luaGlobalString(engine->luaState(), "modal_stacked_phase"), QStringLiteral("after_second"));
	QVERIFY(luaGlobalBoolean(engine->luaState(), "stacked_seen"));
}

void tst_LuaCallbackEngine::workerModalResumeDefersPostModalRuntimeMutations()
{
	WorldRuntime      runtime;
	LuaExecutorWorker executor;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
worker_resume_seen = false
function OnPluginEnable()
  worker_modal_phase = "before"
  local choice = utils.inputbox("choose", "title", "")
  worker_resume_seen = choice == "accepted"
  worker_modal_phase = "after"
  return worker_resume_seen
end
function worker_status(value)
  if worker_resume_seen then
    return "yes"
  end
  return "no"
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines       = {engine};
	request.kind          = LuaBatchDispatchKind::NoArgs;
	request.functionName  = QStringLiteral("OnPluginEnable");
	request.defaultResult = false;

	LuaBatchDispatchResult initialResult;
	dispatchWorkerAndWait(executor, request, initialResult);
	QVERIFY(initialResult.suspended);
	QVERIFY(initialResult.modalResumeId != 0);
	LuaBatchDispatchRequest statusRequest;
	statusRequest.engines      = {engine};
	statusRequest.kind         = LuaBatchDispatchKind::StringInOut;
	statusRequest.functionName = QStringLiteral("worker_status");
	statusRequest.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, statusRequest, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("no"));
	executeDeferredMutations(initialResult);

	LuaBatchDispatchRequest resumeRequest;
	resumeRequest.engines       = {engine};
	resumeRequest.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	resumeRequest.modalResumeId = initialResult.modalResumeId;
	resumeRequest.stringArg     = acceptedModalStringResult(QStringLiteral("accepted"));

	LuaBatchDispatchResult resumedResult;
	dispatchWorkerAndWait(executor, resumeRequest, resumedResult);
	QVERIFY(!resumedResult.suspended);
	QVERIFY(resumedResult.boolResultValid);
	QVERIFY(resumedResult.boolResult);
	QVERIFY(resumedResult.deferredRuntimeMutationBatches.isEmpty());
	executeDeferredMutations(resumedResult);

	dispatchWorkerAndWait(executor, statusRequest, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("yes"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::modalYieldCancelPreventsCallbackContinuation()
{
	WorldRuntime runtime;
	auto         engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
cancel_seen = false
function OnHotspot(flags, hotspot)
  local colour = PickColour(-1)
  cancel_seen = true
  return colour == 42
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines       = {engine};
	request.kind          = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.functionName  = QStringLiteral("OnHotspot");
	request.numberArg1    = 8;
	request.stringArg2    = QStringLiteral("cancel");
	request.defaultResult = false;

	LuaBatchDispatchResult initialResult = executor.dispatchBatch(request);
	QVERIFY(initialResult.suspended);
	QVERIFY(initialResult.modalResumeId != 0);
	QVERIFY(initialResult.hasPendingModalStringRequest);

	LuaBatchDispatchRequest cancelRequest;
	cancelRequest.engines       = {engine};
	cancelRequest.kind          = LuaBatchDispatchKind::CancelSuspendedModalString;
	cancelRequest.modalResumeId = initialResult.modalResumeId;
	static_cast<void>(executor.dispatchBatch(cancelRequest));

	LuaBatchDispatchRequest resumeRequest;
	resumeRequest.engines                      = {engine};
	resumeRequest.kind                         = LuaBatchDispatchKind::ResumeSuspendedModalString;
	resumeRequest.modalResumeId                = initialResult.modalResumeId;
	resumeRequest.stringArg                    = QStringLiteral("42");
	const LuaBatchDispatchResult resumedResult = executor.dispatchBatch(resumeRequest);
	QVERIFY(!resumedResult.suspended);
	QVERIFY(!resumedResult.boolResultValid);
	QVERIFY(!resumedResult.hasFunctionValid);
	QVERIFY(!luaGlobalBoolean(engine->luaState(), "cancel_seen"));
}

void tst_LuaCallbackEngine::callbackCatalogObserverTracksFunctionPresence()
{
	LuaCallbackEngine engine;
	engine.setPluginInfo(QStringLiteral("PLUGIN.ID"), QStringLiteral("Plugin"));

	QString       observedPluginId;
	QSet<QString> observedPresent;
	QSet<QString> observedFunctions;
	int           observerCalls = 0;
	engine.setCallbackCatalogObserver(
	    [&](const QString &pluginId, const QSet<QString> &presentCallbacks, const QSet<QString> &allFunctions)
	    {
		    observedPluginId  = pluginId;
		    observedPresent   = presentCallbacks;
		    observedFunctions = allFunctions;
		    ++observerCalls;
	    });
	engine.setObservedPluginCallbacks({QStringLiteral("on_one"), QStringLiteral("missing")});
	engine.setScriptText(QStringLiteral(R"lua(
function on_one()
end
function helper()
end
)lua"));
	QVERIFY(engine.loadScript());

	QCOMPARE(observedPluginId, QStringLiteral("plugin.id"));
	QVERIFY(observedPresent.contains(QStringLiteral("on_one")));
	QVERIFY(!observedPresent.contains(QStringLiteral("missing")));
	QVERIFY(observedFunctions.contains(QStringLiteral("on_one")));
	QVERIFY(observedFunctions.contains(QStringLiteral("helper")));
	QVERIFY(engine.hasObservedPluginCallback(QStringLiteral("on_one")));
	QVERIFY(!engine.hasObservedPluginCallback(QStringLiteral("missing")));
	QVERIFY(observerCalls >= 2);

	engine.setScriptText(QString());
	QVERIFY(observedPresent.isEmpty());
	QVERIFY(observedFunctions.isEmpty());
}

void tst_LuaCallbackEngine::packageRestrictionsAreAppliedToExistingState()
{
	LuaCallbackEngine engine;
	setEngineScript(engine, QStringLiteral(R"lua(
package.loaders = { "loader1", "loader2", "loader3", "loader4" }
restricted_seen = false
)lua"));

	engine.applyPackageRestrictions(false);
	QVERIFY(engine.executeScript(QStringLiteral(R"lua(
restricted_seen = package.loadlib == nil and
                  package.searchers[3] == nil and package.searchers[4] == nil and
                  package.loaders[3] == nil and package.loaders[4] == nil
)lua"),
	                             QStringLiteral("package restrictions")));
	QVERIFY(luaGlobalBoolean(engine.luaState(), "restricted_seen"));
}

void tst_LuaCallbackEngine::worldLuaFileApisAcceptMixedSeparators()
{
	const QString rootPath = QDir::current().absoluteFilePath(QStringLiteral("qmud_lua_file_path_compat"));
	QDir          root(rootPath);
	if (root.exists())
		QVERIFY(root.removeRecursively());
	QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("sounds"))));

	QFile input(root.filePath(QStringLiteral("sounds/alert.txt")));
	QVERIFY(input.open(QIODevice::WriteOnly | QIODevice::Text));
	QCOMPARE(input.write(QByteArrayLiteral("ok")), qint64{2});
	input.close();

	WorldRuntime runtime;
	runtime.setStartupDirectory(root.absolutePath());
	LuaCallbackEngine engine;
	engine.setWorldRuntime(&runtime);
	setEngineScript(engine, QString());

	const QString script = QStringLiteral(R"lua(
local input = assert(io.open([[C:\MUSHclient\sounds\\\alert.txt]], "r"))
local text = input:read("*a")
input:close()

local output = assert(io.open([[\\legacy\share\sounds\written.txt]], "w"))
output:write("done")
output:close()

local outside_ok = pcall(function()
  return io.open([[..\outside.txt]], "w")
end)

local entries = utils.readdir([[C:\MUSHclient\sounds\\\*.txt]])
mixed_path_io_ok = text == "ok"
mixed_path_readdir_ok = entries["alert.txt"] ~= nil and entries["written.txt"] ~= nil
mixed_path_escape_rejected = not outside_ok
)lua");
	QVERIFY(engine.executeScript(script, QStringLiteral("mixed path compatibility")));
	QVERIFY(luaGlobalBoolean(engine.luaState(), "mixed_path_io_ok"));
	QVERIFY(luaGlobalBoolean(engine.luaState(), "mixed_path_readdir_ok"));
	QVERIFY(luaGlobalBoolean(engine.luaState(), "mixed_path_escape_rejected"));
	QVERIFY(QFile::exists(root.filePath(QStringLiteral("sounds/written.txt"))));
	QVERIFY(root.removeRecursively());
}

void tst_LuaCallbackEngine::worldLuaFileApisUseRuntimeHomeAcrossThreadAffinity()
{
	const QString rootPath = QDir::current().absoluteFilePath(QStringLiteral("qmud_lua_file_path_affinity"));
	QDir          root(rootPath);
	if (root.exists())
		QVERIFY(root.removeRecursively());
	QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("worlds/Tanthul"))));

	QFile input(root.filePath(QStringLiteral("worlds/Tanthul/map.lua")));
	QVERIFY(input.open(QIODevice::WriteOnly | QIODevice::Text));
	QCOMPARE(input.write(QByteArrayLiteral("return 'loaded'")), qint64{15});
	input.close();

	QThread      worker;
	WorldRuntime runtime;
	QThread     *mainThread = QThread::currentThread();
	const auto   cleanup    = qScopeGuard(
	    [&]()
	    {
		    if (runtime.thread() == &worker)
		    {
			    const bool moved = QMetaObject::invokeMethod(
			        &runtime, [&runtime, mainThread]() { runtime.moveToThread(mainThread); },
			        Qt::BlockingQueuedConnection);
			    QVERIFY(moved);
		    }
		    worker.quit();
		    worker.wait();
	    });
	worker.start();
	QVERIFY(worker.isRunning());
	runtime.setStartupDirectory(root.absolutePath());
	runtime.moveToThread(&worker);

	LuaCallbackEngine engine;
	engine.setWorldRuntime(&runtime);
	setEngineScript(engine, QString());

	const QString script = QStringLiteral(R"lua(
local chunk = assert(loadfile([[worlds\Tanthul\map.lua]]))
affinity_path_loaded = chunk() == "loaded"
)lua");
	QVERIFY(engine.executeScript(script, QStringLiteral("runtime home across thread affinity")));
	QVERIFY(luaGlobalBoolean(engine.luaState(), "affinity_path_loaded"));
	QVERIFY(root.removeRecursively());
}

void tst_LuaCallbackEngine::worldLuaFileApisIgnoreProcessQmudHome()
{
	const QString runtimeRootPath = QDir::current().absoluteFilePath(QStringLiteral("qmud_lua_runtime_home"));
	const QString envRootPath     = QDir::current().absoluteFilePath(QStringLiteral("qmud_lua_env_home"));
	QDir          runtimeRoot(runtimeRootPath);
	QDir          envRoot(envRootPath);
	if (runtimeRoot.exists())
		QVERIFY(runtimeRoot.removeRecursively());
	if (envRoot.exists())
		QVERIFY(envRoot.removeRecursively());
	QVERIFY(QDir().mkpath(runtimeRoot.filePath(QStringLiteral("worlds/Tanthul"))));
	QVERIFY(QDir().mkpath(envRoot.filePath(QStringLiteral("worlds/Tanthul"))));

	const auto writeScript = [](const QString &fileName, const QByteArray &content)
	{
		QFile file(fileName);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
			return false;
		return file.write(content) == content.size();
	};
	QVERIFY(writeScript(runtimeRoot.filePath(QStringLiteral("worlds/Tanthul/map.lua")),
	                    QByteArrayLiteral("return 'runtime'")));
	QVERIFY(writeScript(envRoot.filePath(QStringLiteral("worlds/Tanthul/map.lua")),
	                    QByteArrayLiteral("return 'env'")));

	const bool       hadOriginalQmudHome = qEnvironmentVariableIsSet("QMUD_HOME");
	const QByteArray originalQmudHome    = qgetenv("QMUD_HOME");
	qputenv("QMUD_HOME", envRoot.absolutePath().toUtf8());
	const auto restoreQmudHome = qScopeGuard(
	    [hadOriginalQmudHome, originalQmudHome]()
	    {
		    if (hadOriginalQmudHome)
			    qputenv("QMUD_HOME", originalQmudHome);
		    else
			    qunsetenv("QMUD_HOME");
	    });

	WorldRuntime runtime;
	runtime.setStartupDirectory(runtimeRoot.absolutePath());

	LuaCallbackEngine engine;
	engine.setWorldRuntime(&runtime);
	setEngineScript(engine, QString());

	const QString script = QStringLiteral(R"lua(
local chunk = assert(loadfile([[worlds\Tanthul\map.lua]]))
process_qmud_home_ignored = chunk() == "runtime"
)lua");
	QVERIFY(engine.executeScript(script, QStringLiteral("ignore process QMUD_HOME")));
	QVERIFY(luaGlobalBoolean(engine.luaState(), "process_qmud_home_ignored"));

	runtime.setStartupDirectory(QString());
	LuaCallbackEngine missingHomeEngine;
	missingHomeEngine.setWorldRuntime(&runtime);
	setEngineScript(missingHomeEngine, QString());

	const QString missingHomeScript = QStringLiteral(R"lua(
local ok = pcall(function()
  assert(loadfile([[worlds\Tanthul\map.lua]]))
end)
missing_runtime_home_does_not_use_env = not ok
)lua");
	QVERIFY(missingHomeEngine.executeScript(missingHomeScript,
	                                        QStringLiteral("missing runtime home ignores env")));
	QVERIFY(luaGlobalBoolean(missingHomeEngine.luaState(), "missing_runtime_home_does_not_use_env"));
	QVERIFY(runtimeRoot.removeRecursively());
	QVERIFY(envRoot.removeRecursively());
}

void tst_LuaCallbackEngine::luaVisiblePathApisReturnRelativePosix()
{
	const QString rootPath = QDir::current().absoluteFilePath(QStringLiteral("qmud_lua_visible_path_api"));
	QDir          root(rootPath);
	if (root.exists())
		QVERIFY(root.removeRecursively());
	QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("worlds/foo"))));
	QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("logs"))));
	QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("worlds/plugins/state"))));
	QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("locale"))));
	QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("fonts"))));
	QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("prefs"))));
	QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("worlds/browse"))));

	const QString previousCurrentPath = QDir::currentPath();
	QVERIFY(QDir::setCurrent(root.absolutePath()));
	[[maybe_unused]] const auto restoreCurrentPath =
	    qScopeGuard([previousCurrentPath] { QDir::setCurrent(previousCurrentPath); });

	WorldRuntime runtime;
	runtime.setStartupDirectory(root.absolutePath());
	runtime.setWorldAttribute(QStringLiteral("new_activity_sound"),
	                          QStringLiteral(R"(C:\MUSHclient\sounds\activity.wav)"));
	runtime.setWorldAttribute(QStringLiteral("script_editor"),
	                          QStringLiteral(R"(C:\MUSHclient\worlds\foo\editor.lua)"));
	runtime.setWorldAttribute(QStringLiteral("script_filename"),
	                          QStringLiteral(R"(C:\MUSHclient\worlds\foo\script.lua)"));
	runtime.setWorldAttribute(QStringLiteral("auto_log_file_name"),
	                          QStringLiteral(R"(C:\MUSHclient\logs\auto.log)"));
	runtime.setWorldAttribute(QStringLiteral("beep_sound"),
	                          QStringLiteral(R"(C:\MUSHclient\sounds\beep.wav)"));
	runtime.setWorldAttribute(QStringLiteral("foreground_image"),
	                          QStringLiteral(R"(C:\MUSHclient\worlds\foo\foreground.png)"));
	runtime.setWorldAttribute(QStringLiteral("background_image"),
	                          QStringLiteral(R"(C:\MUSHclient\worlds\foo\background.png)"));
	runtime.setWorldFilePath(root.filePath(QStringLiteral("worlds/foo/test.mcl")));
	runtime.setDefaultWorldDirectory(QStringLiteral("C:/MUSHclient/worlds/"));
	runtime.setDefaultLogDirectory(root.filePath(QStringLiteral("logs")));
	runtime.setPluginsDirectory(QStringLiteral("C:/MUSHclient/worlds/plugins/"));
	runtime.setTranslatorFile(root.filePath(QStringLiteral("locale/EN.lua")));
	runtime.setPreferencesDatabaseName(root.filePath(QStringLiteral("prefs/qmud.db")));
	runtime.setFileBrowsingDirectory(root.filePath(QStringLiteral("worlds/browse")));
	runtime.setStateFilesDirectory(QStringLiteral("C:/MUSHclient/worlds/plugins/state/"));
	QCOMPARE(runtime.openLog(root.filePath(QStringLiteral("logs/current.log")), false), eOK);

	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QString());

	auto snapshot                       = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasWorldAttributeSnapshot = true;
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("new_activity_sound"),
	                                         QStringLiteral(R"(C:\MUSHclient\sounds\activity.wav)"));
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("script_editor"),
	                                         QStringLiteral(R"(C:\MUSHclient\worlds\foo\editor.lua)"));
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("script_filename"),
	                                         QStringLiteral(R"(C:\MUSHclient\worlds\foo\script.lua)"));
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("auto_log_file_name"),
	                                         QStringLiteral(R"(C:\MUSHclient\logs\auto.log)"));
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("beep_sound"),
	                                         QStringLiteral(R"(C:\MUSHclient\sounds\beep.wav)"));
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("foreground_image"),
	                                         QStringLiteral(R"(C:\MUSHclient\worlds\foo\foreground.png)"));
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("background_image"),
	                                         QStringLiteral(R"(C:\MUSHclient\worlds\foo\background.png)"));
	snapshot->hasRuntimeCountersSnapshot = true;
	snapshot->runtimeCounterValues.insert(QStringLiteral("logFileName"),
	                                      root.filePath(QStringLiteral("logs/current.log")));
	snapshot->runtimeCounterValues.insert(QStringLiteral("worldFilePath"),
	                                      root.filePath(QStringLiteral("worlds/foo/test.mcl")));
	snapshot->runtimeCounterValues.insert(QStringLiteral("defaultWorldDirectory"),
	                                      QStringLiteral("C:/MUSHclient/worlds/"));
	snapshot->runtimeCounterValues.insert(QStringLiteral("defaultLogDirectory"),
	                                      root.filePath(QStringLiteral("logs")));
	snapshot->runtimeCounterValues.insert(QStringLiteral("pluginsDirectory"),
	                                      QStringLiteral("C:/MUSHclient/worlds/plugins/"));
	snapshot->runtimeCounterValues.insert(QStringLiteral("startupDirectory"), root.absolutePath());
	snapshot->runtimeCounterValues.insert(QStringLiteral("translatorFile"),
	                                      root.filePath(QStringLiteral("locale/EN.lua")));
	snapshot->runtimeCounterValues.insert(QStringLiteral("firstSpecialFontPath"), QString());
	snapshot->runtimeCounterValues.insert(QStringLiteral("preferencesDatabaseName"),
	                                      root.filePath(QStringLiteral("prefs/qmud.db")));
	snapshot->runtimeCounterValues.insert(QStringLiteral("fileBrowsingDirectory"),
	                                      root.filePath(QStringLiteral("worlds/browse")));
	snapshot->runtimeCounterValues.insert(QStringLiteral("stateFilesDirectory"),
	                                      QStringLiteral("C:/MUSHclient/worlds/plugins/state/"));

	const QString           script = QStringLiteral(R"lua(
local values = {
  GetInfo(9),
  GetInfo(10),
  GetInfo(35),
  GetInfo(40),
  GetInfo(50),
  GetInfo(51),
  GetInfo(54),
  GetInfo(57),
  GetInfo(58),
  GetInfo(59),
  GetInfo(60),
  GetInfo(64),
  GetInfo(66),
  GetInfo(67),
  GetInfo(68),
  GetInfo(69),
  GetInfo(74),
  GetInfo(76),
  GetInfo(78),
  GetInfo(79),
  GetInfo(82),
  GetInfo(84),
  GetInfo(85),
}
path_summary = table.concat(values, "|")
path_no_backslashes = not path_summary:find("\\", 1, true)
getinfo_56 = tostring(GetInfo(56))
getinfo_56_ok = #getinfo_56 > 0 and getinfo_56:sub(-1) == "/" and not getinfo_56:find("\\", 1, true)
local info = utils.info()
utils_info_summary = tostring(info.app_directory) .. "|" .. tostring(info.current_directory)
utils_info_ok = utils_info_summary == "./|./"
)lua");

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::ExecuteScript;
	request.stringArg             = script;
	request.stringArg2            = QStringLiteral("path visible api snapshot");
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result = executor.dispatchBatch(request);
	QVERIFY(result.boolResultValid);
	QVERIFY(result.boolResult);
	executeDeferredMutations(result);

	const QString expected =
	    QStringList{
	        QStringLiteral("sounds/activity.wav"),
	        QStringLiteral("worlds/foo/editor.lua"),
	        QStringLiteral("worlds/foo/script.lua"),
	        QStringLiteral("logs/auto.log"),
	        QStringLiteral("sounds/beep.wav"),
	        QStringLiteral("logs/current.log"),
	        QStringLiteral("worlds/foo/test.mcl"),
	        QStringLiteral("worlds/"),
	        QStringLiteral("logs/"),
	        QStringLiteral("./"),
	        QStringLiteral("worlds/plugins/"),
	        QStringLiteral("./"),
	        QStringLiteral("./"),
	        QStringLiteral("worlds/foo/"),
	        QStringLiteral("./"),
	        QStringLiteral("locale/EN.lua"),
	        QStringLiteral("sounds/"),
	        QString(),
	        QStringLiteral("worlds/foo/foreground.png"),
	        QStringLiteral("worlds/foo/background.png"),
	        QStringLiteral("prefs/qmud.db"),
	        QStringLiteral("worlds/browse/"),
	        QStringLiteral("worlds/plugins/state/"),
	    }
	        .join(QLatin1Char('|'));
	const QString expectedGetInfo56 =
	    QMudPluginPathUtils::normalizeSeparators(root.absolutePath()) + QLatin1Char('/');
	QCOMPARE(luaGlobalString(engine->luaState(), "path_summary"), expected);
	QVERIFY(luaGlobalBoolean(engine->luaState(), "path_no_backslashes"));
	QCOMPARE(luaGlobalString(engine->luaState(), "getinfo_56"), expectedGetInfo56);
	QVERIFY(luaGlobalBoolean(engine->luaState(), "getinfo_56_ok"));
	QCOMPARE(luaGlobalString(engine->luaState(), "utils_info_summary"), QStringLiteral("./|./"));
	QVERIFY(luaGlobalBoolean(engine->luaState(), "utils_info_ok"));
	QVERIFY(QDir::setCurrent(previousCurrentPath));
	QVERIFY(root.removeRecursively());
}

void tst_LuaCallbackEngine::utilsMultiListBoxAcceptsMushclientArgumentOrder()
{
	LuaCallbackEngine engine;
	setEngineScript(engine, QStringLiteral(R"lua(
local ok, result = pcall(function()
  return utils.multilistbox("Choose channels", "New Tab", { tell = "tell", shout = "shout" }, { tell = true })
end)
multilistbox_signature_ok = ok
multilistbox_error = tostring(result)
multilistbox_no_host_returns_nil = result == nil
)lua"));

	const QByteArray multilistboxError = luaGlobalString(engine.luaState(), "multilistbox_error").toUtf8();
	QVERIFY2(luaGlobalBoolean(engine.luaState(), "multilistbox_signature_ok"), multilistboxError.constData());
	QVERIFY(luaGlobalBoolean(engine.luaState(), "multilistbox_no_host_returns_nil"));
}

void tst_LuaCallbackEngine::deferredRuntimeMutationBatchesPreserveOrderAndOwnership()
{
	LuaCallbackEngine engine;
	int               value   = 0;
	auto             *runtime = reinterpret_cast<WorldRuntime *>(static_cast<quintptr>(1));
	engine.appendDeferredRuntimeMutationBatch(runtime, {std::function<void()>([&value] { value += 1; }),
	                                                    std::function<void()>([&value] { value *= 10; })});

	QVector<LuaDeferredRuntimeMutationBatch> batches = engine.takeDeferredRuntimeMutationBatches();
	QCOMPARE(batches.size(), 1);
	QVERIFY(batches.first().runtime == runtime);
	QCOMPARE(batches.first().mutations.size(), 2);
	for (auto &mutation : batches.first().mutations)
		mutation();
	QCOMPARE(value, 10);
	QVERIFY(engine.takeDeferredRuntimeMutationBatches().isEmpty());

	LuaDeferredRuntimeMutationBatch nested;
	nested.runtime = runtime;
	nested.mutations.push_back([&value] { value += 5; });
	QVERIFY(!LuaCallbackEngine::appendDeferredRuntimeMutationBatchToActiveCallback(nested));
	QCOMPARE(nested.mutations.size(), 1);
}

void tst_LuaCallbackEngine::directExecutorDispatchesRealEngines()
{
	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	setEngineScript(*engine, QStringLiteral(R"lua(
function stop_false(value)
  return value ~= "stop"
end
function string_handled(value)
  handled_value = value
  return false
end
function bytes_inout(value)
  return value .. ":bytes"
end
function string_inout(value)
  return value .. ":string"
end
function count_utf8(number, one, two, three)
  count_seen = tostring(number) .. ":" .. one .. two .. three
  return true
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringStopOnFalse;
	request.functionName          = QStringLiteral("stop_false");
	request.stringArg             = QStringLiteral("stop");
	request.defaultResult         = true;
	LuaBatchDispatchResult result = executor.dispatchBatch(request);
	QVERIFY(result.boolResultValid);
	QVERIFY(!result.boolResult);

	request.kind      = LuaBatchDispatchKind::StringHandled;
	request.stringArg = QStringLiteral("handled");
	result            = executor.dispatchBatch(request);
	QVERIFY(result.boolResultValid);
	QVERIFY(result.boolResult);

	request.kind     = LuaBatchDispatchKind::BytesInOut;
	request.bytesArg = QByteArray("payload");
	request.stringArg.clear();
	request.functionName = QStringLiteral("bytes_inout");
	result               = executor.dispatchBatch(request);
	QCOMPARE(result.bytesResult, QByteArray("payload:bytes"));

	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("string_inout");
	request.stringArg    = QStringLiteral("payload");
	result               = executor.dispatchBatch(request);
	QCOMPARE(result.stringResult, QStringLiteral("payload:string"));

	request.kind         = LuaBatchDispatchKind::NumberAndUtf8StringsCount;
	request.functionName = QStringLiteral("count_utf8");
	request.numberArg1   = 3;
	request.bytesArg     = QByteArray("a");
	request.bytesArg2    = QByteArray("b");
	request.bytesArg3    = QByteArray("c");
	result               = executor.dispatchBatch(request);
	QVERIFY(result.countResultValid);
	QCOMPARE(result.countResult, 1);
	QCOMPARE(luaGlobalString(engine->luaState(), "count_seen"), QStringLiteral("3:abc"));
}

void tst_LuaCallbackEngine::callPluginMarshallingUsesTargetEngineState()
{
	LuaCallbackEngine target;
	setEngineScript(target, QStringLiteral(R"lua(
plugin = {}
function plugin.echo(value, number)
  return value .. ":" .. tostring(number), true
end
)lua"));

	LuaStatePtr caller = makeLuaState();
	lua_pushstring(caller.get(), "input");
	lua_pushnumber(caller.get(), 42);
	const CallPluginLuaMarshallingResult result =
	    target.callPluginLuaWithMarshalling(caller.get(), QStringLiteral("plugin.echo"), 1);
	QCOMPARE(result.error, CallPluginLuaMarshallingError::None);
	QCOMPARE(result.returnCount, 2);
	QCOMPARE(lua_gettop(caller.get()), 4);
	QCOMPARE(QString::fromUtf8(lua_tostring(caller.get(), 3)), QStringLiteral("input:42.0"));
	QVERIFY(lua_toboolean(caller.get(), 4) != 0);
}

void tst_LuaCallbackEngine::noArgsDispatchReportsCallbackFailure()
{
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function successful_install()
  return true
end
function failed_install()
  return false
end
)lua"));

	LuaBatchDispatchRequest request;
	request.engines       = {engine};
	request.kind          = LuaBatchDispatchKind::NoArgs;
	request.functionName  = QStringLiteral("successful_install");
	request.defaultResult = true;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(result.boolResultValid);
	QVERIFY(result.boolResult);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(result.hasFunction);

	request.functionName = QStringLiteral("failed_install");
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(result.boolResultValid);
	QVERIFY(!result.boolResult);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(result.hasFunction);

	request.functionName = QStringLiteral("missing_install");
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(result.boolResultValid);
	QVERIFY(result.boolResult);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(!result.hasFunction);

	request.kind    = LuaBatchDispatchKind::TeardownEnginesMany;
	request.engines = {engine};
	dispatchWorkerAndWait(executor, request);
}

void tst_LuaCallbackEngine::workerDispatchesPluginLifecycleCallbacksOnRealEngines()
{
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
lifecycle = {}
function OnPluginInstall()
  table.insert(lifecycle, "install")
end
function OnPluginEnable()
  table.insert(lifecycle, "enable")
end
function OnPluginDisable()
  table.insert(lifecycle, "disable")
end
function OnPluginClose()
  table.insert(lifecycle, "close")
end
function lifecycle_join(value)
  return table.concat(lifecycle, ",")
end
)lua"));

	LuaBatchDispatchRequest request;
	request.engines = {engine};
	request.kind    = LuaBatchDispatchKind::NoArgs;
	for (const QString &functionName : {QStringLiteral("OnPluginInstall"), QStringLiteral("OnPluginEnable"),
	                                    QStringLiteral("OnPluginDisable"), QStringLiteral("OnPluginClose")})
	{
		request.functionName = functionName;
		dispatchWorkerAndWait(executor, request);
	}

	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("lifecycle_join");
	request.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("install,enable,disable,close"));

	request.kind    = LuaBatchDispatchKind::TeardownEnginesMany;
	request.engines = {engine};
	dispatchWorkerAndWait(executor, request);
	QVERIFY(engine->luaState() == nullptr);
}

void tst_LuaCallbackEngine::workerCallbackBatchCapturesOutputMiniWindowAndSaveStateMutations()
{
	WorldRuntime runtime;
	QStringList  outputTexts;
	QList<bool>  outputNewLines;
	QObject::connect(&runtime, &WorldRuntime::outputRequested, &runtime,
	                 [&](const QString &text, const bool newLine, const bool)
	                 {
		                 outputTexts.push_back(text);
		                 outputNewLines.push_back(newLine);
	                 });
	QObject::connect(
	    &runtime, &WorldRuntime::outputStyledRequested, &runtime,
	    [&](const QString &text, const QVector<WorldRuntime::StyleSpan> &, const bool newLine, const bool)
	    {
		    outputTexts.push_back(text);
		    outputNewLines.push_back(newLine);
	    });
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
batch_seen = ""
function OnPluginEnable()
  Note("batch-note")
  local create_status = WindowCreate("batch", 10, 20, 40, 50, 0, 0, 0)
  local resize_status = WindowResize("batch", 64, 32, 0)
  local position_status = WindowPosition("batch", 12, 24, 0, 0)
  local hotspot_status = WindowAddHotspot("batch", "drag", 1, 2, 9, 10, "", "", "", "", "", "tip", 0, 0)
  local save_status, save_request_id = SaveState()
  batch_seen = table.concat({
    tostring(create_status == eOK),
    tostring(resize_status == eOK),
    tostring(position_status == eOK),
    tostring(hotspot_status == eOK),
    tostring(save_status == eOK),
    tostring(save_request_id ~= nil)
  }, "|")
end
function batch_status(value)
  return batch_seen
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("OnPluginEnable");
	LuaBatchDispatchResult callbackResult;
	dispatchWorkerAndWait(executor, request, callbackResult);

	int mutationCount = 0;
	for (const LuaDeferredRuntimeMutationBatch &batch : callbackResult.deferredRuntimeMutationBatches)
		mutationCount += safeQSizeToInt(batch.mutations.size());
	QVERIFY2(mutationCount >= 1, qPrintable(QString::number(mutationCount)));

	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("batch_status");
	request.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, request, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("true|true|true|true|true|true"));

	executeDeferredMutations(callbackResult);
	QVERIFY(outputTexts.contains(QStringLiteral("batch-note")));
	QVERIFY(runtime.windowList().contains(QStringLiteral("batch")));
	QCOMPARE(runtime.windowInfo(QStringLiteral("batch"), 1).toInt(), 12);
	QCOMPARE(runtime.windowInfo(QStringLiteral("batch"), 2).toInt(), 24);
	QCOMPARE(runtime.windowInfo(QStringLiteral("batch"), 3).toInt(), 64);
	QCOMPARE(runtime.windowInfo(QStringLiteral("batch"), 4).toInt(), 32);
	QVERIFY(runtime.windowHotspotList(QStringLiteral("batch")).contains(QStringLiteral("drag")));
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerColourOutputMatchesMushclientGroupingAndNewlineSemantics()
{
	WorldRuntime runtime;
	QStringList  outputTexts;
	QList<bool>  outputNewLines;
	QObject::connect(
	    &runtime, &WorldRuntime::outputStyledRequested, &runtime,
	    [&](const QString &text, const QVector<WorldRuntime::StyleSpan> &, const bool newLine, const bool)
	    {
		    outputTexts.push_back(text);
		    outputNewLines.push_back(newLine);
	    });
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function OnPluginEnable()
  ColourNote("red", "black", "note-a",
             "green", "black", "note-b",
             "blue", "black", "note-c")
  ColourTell("cyan", "black", "tell-a",
             "yellow", "black", "tell-b")
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	executeDeferredMutations(result);

	QCOMPARE(outputTexts,
	         QStringList({QStringLiteral("note-a"), QStringLiteral("note-b"), QStringLiteral("note-c"),
	                      QStringLiteral("tell-a"), QStringLiteral("tell-b")}));
	QCOMPARE(outputNewLines, QList<bool>({false, false, true, false, false}));
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerColourOutputPreservesIndexedNoteColour()
{
	WorldRuntime runtime;
	runtime.setNoteTextColour(1);
	const QColor expectedChangedFore = colorFromPackedValue(runtime.noteColourFore());
	const QColor expectedChangedBack = colorFromPackedValue(runtime.noteColourBack());
	runtime.setNoteTextColour(5);
	QCOMPARE(runtime.notesInRgb(), false);
	QCOMPARE(runtime.noteTextColour(), 4);
	const QColor                                expectedFore = colorFromPackedValue(runtime.noteColourFore());
	const QColor                                expectedBack = colorFromPackedValue(runtime.noteColourBack());
	const WorldRuntime::RuntimeCountersSnapshot counters     = runtime.runtimeCountersSnapshot(false);
	QCOMPARE(colorFromPackedValue(counters.noteColourFore), expectedFore);
	QCOMPARE(colorFromPackedValue(counters.noteColourBack), expectedBack);

	QStringList                               outputTexts;
	QVector<QVector<WorldRuntime::StyleSpan>> outputSpans;
	QObject::connect(
	    &runtime, &WorldRuntime::outputStyledRequested, &runtime,
	    [&](const QString &text, const QVector<WorldRuntime::StyleSpan> &spans, const bool, const bool)
	    {
		    outputTexts.push_back(text);
		    outputSpans.push_back(spans);
	    });

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function OnPluginEnable()
  ColourNote("", "", "note")
  ColourTell("not-a-colour", "", "tell")
  SetNoteColour(1)
  Note("after")
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	executeDeferredMutations(result);

	QCOMPARE(outputTexts,
	         QStringList({QStringLiteral("note"), QStringLiteral("tell"), QStringLiteral("after")}));
	QCOMPARE(outputSpans.size(), 3);
	for (const QVector<WorldRuntime::StyleSpan> &spans : std::as_const(outputSpans))
		QVERIFY(!spans.isEmpty());
	QCOMPARE(outputSpans.at(0).constFirst().fore, expectedFore);
	QCOMPARE(outputSpans.at(0).constFirst().back, expectedBack);
	QCOMPARE(outputSpans.at(1).constFirst().fore, expectedFore);
	QCOMPARE(outputSpans.at(1).constFirst().back, expectedBack);
	QCOMPARE(outputSpans.at(2).constFirst().fore, expectedChangedFore);
	QCOMPARE(outputSpans.at(2).constFirst().back, expectedChangedBack);
	QCOMPARE(runtime.notesInRgb(), false);
	QCOMPARE(runtime.noteTextColour(), 0);
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::normalColourDefaultsMatchMushclientAcrossRuntimeAndCallbackPaths()
{
	WorldRuntime runtime;
	runtime.setColours({});

	const QString expected = QStringList{QString::number(static_cast<long>(qmudRgb(0, 0, 0))),
	                                     QString::number(static_cast<long>(qmudRgb(128, 0, 0))),
	                                     QString::number(static_cast<long>(qmudRgb(0, 128, 0))),
	                                     QString::number(static_cast<long>(qmudRgb(128, 128, 0))),
	                                     QString::number(static_cast<long>(qmudRgb(0, 0, 128))),
	                                     QString::number(static_cast<long>(qmudRgb(128, 0, 128))),
	                                     QString::number(static_cast<long>(qmudRgb(0, 128, 128))),
	                                     QString::number(static_cast<long>(qmudRgb(192, 192, 192))),
	                                     QStringLiteral("0"),
	                                     QStringLiteral("0")}
	                             .join(QLatin1Char('|'));

	QStringList   runtimeValues;
	for (int index = 1; index <= 8; ++index)
		runtimeValues.push_back(QString::number(runtime.normalColour(index)));
	runtimeValues.push_back(QString::number(runtime.normalColour(0)));
	runtimeValues.push_back(QString::number(runtime.normalColour(9)));
	QCOMPARE(runtimeValues.join(QLatin1Char('|')), expected);

	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
function normal_colour_status(value)
  local values = {}
  local function add(value)
    values[#values + 1] = string.format("%.0f", value)
  end
  for index = 1, 8 do
    add(GetNormalColour(index))
  end
  add(GetNormalColour(0))
  add(GetNormalColour(9))
  return table.concat(values, "|")
end
function selected_colour_status(value)
  return string.format("%.0f|%.0f", GetNormalColour(2), GetBoldColour(2))
end
function same_colour_note_status(value)
  SetNoteColour(0)
  return string.format("%.0f|%.0f", GetNoteColourFore(), GetNoteColourBack())
end
function indexed_note_palette_status(value)
  SetNoteColour(1)
  SetCustomColourText(1, 460809)
  SetCustomColourBackground(1, 263430)
  return string.format("%.0f|%.0f", GetNoteColourFore(), GetNoteColourBack())
end
function same_colour_normal_palette_status(value)
  SetNoteColour(0)
  SetNormalColour(8, 855051)
  SetNormalColour(1, 1052430)
  return string.format("%.0f|%.0f", GetNoteColourFore(), GetNoteColourBack())
end
function same_colour_custom16_option_status(value)
  SetNoteColour(0)
  SetCustomColourText(16, 1118739)
  SetCustomColourBackground(16, 1316118)
  SetOption("custom_16_is_default_colour", 1)
  return string.format("%.0f|%.0f", GetNoteColourFore(), GetNoteColourBack())
end
function invalid_colour_cache_status(value)
  SetNormalColour(0, 123)
  SetNormalColour(9, 456)
  SetBoldColour(0, 123)
  SetBoldColour(9, 456)
  SetCustomColourText(0, 123)
  SetCustomColourText(17, 456)
  SetCustomColourBackground(0, 123)
  SetCustomColourBackground(17, 456)
  SetNormalColour(2, 16909060)
  return string.format("%.0f|%.0f|%.0f|%.0f|%.0f|%.0f|%.0f|%.0f|%.0f",
    GetNormalColour(0), GetNormalColour(9), GetBoldColour(0), GetBoldColour(9),
    GetCustomColourText(0), GetCustomColourText(17),
    GetCustomColourBackground(0), GetCustomColourBackground(17), GetNormalColour(2))
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines                       = {engine};
	request.kind                          = LuaBatchDispatchKind::StringInOut;
	request.functionName                  = QStringLiteral("normal_colour_status");
	request.stringArg                     = QStringLiteral("ignored");
	request.miniWindowSnapshotArg         = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult snapshotResult = executor.dispatchBatch(request);
	QCOMPARE(snapshotResult.stringResult, expected);

	request.functionName                  = QStringLiteral("same_colour_note_status");
	request.miniWindowSnapshotArg         = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult sameNoteResult = executor.dispatchBatch(request);
	QCOMPARE(sameNoteResult.stringResult, QStringLiteral("%1|%2")
	                                          .arg(static_cast<long>(qmudRgb(192, 192, 192)))
	                                          .arg(static_cast<long>(qmudRgb(0, 0, 0))));

	request.functionName                     = QStringLiteral("indexed_note_palette_status");
	request.miniWindowSnapshotArg            = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult indexedNoteResult = executor.dispatchBatch(request);
	QCOMPARE(indexedNoteResult.stringResult, QStringLiteral("%1|%2")
	                                             .arg(static_cast<long>(qmudRgb(9, 8, 7)))
	                                             .arg(static_cast<long>(qmudRgb(6, 5, 4))));

	request.functionName                    = QStringLiteral("same_colour_normal_palette_status");
	request.miniWindowSnapshotArg           = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult sameColourResult = executor.dispatchBatch(request);
	QCOMPARE(sameColourResult.stringResult, QStringLiteral("%1|%2")
	                                            .arg(static_cast<long>(qmudRgb(11, 12, 13)))
	                                            .arg(static_cast<long>(qmudRgb(14, 15, 16))));

	request.functionName                        = QStringLiteral("same_colour_custom16_option_status");
	request.miniWindowSnapshotArg               = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult custom16OptionResult = executor.dispatchBatch(request);
	QCOMPARE(custom16OptionResult.stringResult, QStringLiteral("%1|%2")
	                                                .arg(static_cast<long>(qmudRgb(19, 18, 17)))
	                                                .arg(static_cast<long>(qmudRgb(22, 21, 20))));

	request.functionName                      = QStringLiteral("invalid_colour_cache_status");
	request.miniWindowSnapshotArg             = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult invalidCacheResult = executor.dispatchBatch(request);
	QCOMPARE(invalidCacheResult.stringResult,
	         QStringLiteral("0|0|0|0|0|0|0|0|%1").arg(static_cast<long>(qmudRgb(4, 3, 2))));

	runtime.setNormalColour(2, qmudRgb(1, 2, 3));
	runtime.setAnsiColour(true, 2, QColor(1, 2, 3));
	QCOMPARE(runtime.normalColour(2), static_cast<long>(qmudRgb(1, 2, 3)));
	QCOMPARE(runtime.ansiColour(false, 2), QColor(1, 2, 3));
	QCOMPARE(runtime.ansiColour(true, 2), QColor(1, 2, 3));

	request.functionName                  = QStringLiteral("selected_colour_status");
	request.miniWindowSnapshotArg         = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult selectedResult = executor.dispatchBatch(request);
	QCOMPARE(selectedResult.stringResult, QStringLiteral("%1|%2")
	                                          .arg(static_cast<long>(qmudRgb(1, 2, 3)))
	                                          .arg(static_cast<long>(qmudRgb(1, 2, 3))));
}

void tst_LuaCallbackEngine::emptyColourTellDoesNotMutateCallbackOutputCache()
{
	WorldRuntime runtime;

	QStringList  outputTexts;
	QObject::connect(&runtime, &WorldRuntime::outputStyledRequested, &runtime,
	                 [&](const QString &text, const QVector<WorldRuntime::StyleSpan> &, const bool,
	                     const bool) { outputTexts.push_back(text); });

	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
function empty_colour_tell_status(value)
  local before = GetLinesInBufferCount()
  ColourTell("", "", "")
  return string.format("%.0f|%.0f|%s", before, GetLinesInBufferCount(), GetRecentLines(1))
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines                       = {engine};
	request.kind                          = LuaBatchDispatchKind::StringInOut;
	request.functionName                  = QStringLiteral("empty_colour_tell_status");
	request.stringArg                     = QStringLiteral("ignored");
	request.miniWindowSnapshotArg         = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult callbackResult = executor.dispatchBatch(request);

	QCOMPARE(callbackResult.stringResult, QStringLiteral("0|0|"));
	executeDeferredMutations(callbackResult);
	QVERIFY(outputTexts.isEmpty());
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 0);
}

void tst_LuaCallbackEngine::colourTellIgnoresTrailingLuaGsubReturnAndKeepsFollowingNote()
{
	WorldRuntime runtime;
	QStringList  outputTexts;
	QList<bool>  outputNewLines;
	QObject::connect(&runtime, &WorldRuntime::outputRequested, &runtime,
	                 [&](const QString &text, const bool newLine, const bool)
	                 {
		                 outputTexts.push_back(text);
		                 outputNewLines.push_back(newLine);
	                 });
	QObject::connect(
	    &runtime, &WorldRuntime::outputStyledRequested, &runtime,
	    [&](const QString &text, const QVector<WorldRuntime::StyleSpan> &, const bool newLine, const bool)
	    {
		    outputTexts.push_back(text);
		    outputNewLines.push_back(newLine);
	    });
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function OnPluginEnable()
  local profit = 10000
  Tell("You made ")
  ColourTell("yellow", "black", tostring(profit):reverse():gsub("(%d%d%d)", "%1,"):reverse():gsub("^,", ""))
  Note(" gold!")
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("OnPluginEnable");
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	executeDeferredMutations(result);

	QCOMPARE(outputTexts,
	         QStringList({QStringLiteral("You made "), QStringLiteral("10,000"), QStringLiteral(" gold!")}));
	QCOMPARE(outputNewLines, QList<bool>({false, false, true}));
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::executeScriptNoteUsesRuntimeNoteColour()
{
	WorldRuntime                     runtime;
	QString                          outputText;
	QVector<WorldRuntime::StyleSpan> outputSpans;
	QObject::connect(
	    &runtime, &WorldRuntime::outputStyledRequested, &runtime,
	    [&](const QString &text, const QVector<WorldRuntime::StyleSpan> &spans, const bool, const bool)
	    {
		    outputText  = text;
		    outputSpans = spans;
	    });
	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QString());

	auto snapshot                        = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasRuntimeCountersSnapshot = true;
	snapshot->runtimeCounterValues.insert(QStringLiteral("notesInRgb"), true);
	snapshot->runtimeCounterValues.insert(QStringLiteral("noteColourFore"),
	                                      QVariant::fromValue<qlonglong>(0x00FFFF00));
	snapshot->runtimeCounterValues.insert(QStringLiteral("noteColourBack"),
	                                      QVariant::fromValue<qlonglong>(0));
	snapshot->runtimeCounterValues.insert(QStringLiteral("noteTextColour"), -1);

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::ExecuteScript;
	request.stringArg             = QStringLiteral("Note('test')");
	request.stringArg2            = QStringLiteral("immediate note colour");
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result = executor.dispatchBatch(request);
	QVERIFY(result.boolResultValid);
	QVERIFY(result.boolResult);
	executeDeferredMutations(result);

	QCOMPARE(outputText, QStringLiteral("test"));
	QVERIFY(!outputSpans.isEmpty());
	QCOMPARE(outputSpans.constFirst().fore, QColor(0, 255, 255));
	QCOMPARE(outputSpans.constFirst().back, QColor(Qt::black));
}

void tst_LuaCallbackEngine::selfPluginInfoMetadataFallsThroughToRuntime()
{
	WorldRuntime         runtime;

	WorldRuntime::Plugin plugin;
	plugin.attributes.insert(QStringLiteral("id"), QStringLiteral("Plugin.Id"));
	plugin.attributes.insert(QStringLiteral("name"), QStringLiteral("Runtime Plugin Name"));
	plugin.attributes.insert(QStringLiteral("author"), QStringLiteral("Runtime Author"));
	plugin.attributes.insert(QStringLiteral("language"), QStringLiteral("lua"));
	plugin.attributes.insert(QStringLiteral("purpose"), QStringLiteral("Runtime Purpose"));
	plugin.description = QStringLiteral("Runtime Description");
	plugin.script      = QStringLiteral("Runtime Script");
	plugin.source      = QStringLiteral("worlds/plugins/runtime_plugin.xml");
	plugin.directory   = QStringLiteral("worlds/plugins/");
	runtime.pluginsMutable().push_back(plugin);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local plugin_id = "Plugin.Id"
	  self_info = table.concat({
	    GetPluginInfo(plugin_id, 1) or "",
	    GetPluginInfo(plugin_id, 2) or "",
	    GetPluginInfo(plugin_id, 3) or "",
	    GetPluginInfo(plugin_id, 4) or "",
	    GetPluginInfo(plugin_id, 5) or "",
	    GetPluginInfo(plugin_id, 6) or "",
	    GetPluginInfo(plugin_id, 7) or "",
	    GetPluginInfo(plugin_id, 8) or "",
	    GetPluginInfo(plugin_id, 20) or ""
	  }, "|")
	end
	function self_info_status(value)
	  return self_info
	end
	)lua"),
	                       &runtime);

	const QString pluginKey = QStringLiteral("plugin.id");
	auto          snapshot  = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->pluginIdsSnapshot.push_back(pluginKey);
	snapshot->pluginNamesById.insert(pluginKey, QStringLiteral("Runtime Plugin Name"));
	snapshot->pluginEnabledById.insert(pluginKey, true);
	snapshot->pluginDirectoriesById.insert(pluginKey, QStringLiteral("worlds/plugins/"));
	auto &pluginInfo = snapshot->pluginInfoValuesById[pluginKey];
	pluginInfo.insert(1, QStringLiteral("Runtime Plugin Name"));
	pluginInfo.insert(2, QStringLiteral("Runtime Author"));
	pluginInfo.insert(3, QStringLiteral("Runtime Description"));
	pluginInfo.insert(4, QStringLiteral("Runtime Script"));
	pluginInfo.insert(5, QStringLiteral("lua"));
	pluginInfo.insert(6, QStringLiteral("worlds/plugins/runtime_plugin.xml"));
	pluginInfo.insert(7, QStringLiteral("Plugin.Id"));
	pluginInfo.insert(8, QStringLiteral("Runtime Purpose"));
	pluginInfo.insert(20, QStringLiteral("worlds/plugins/"));

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = snapshot;
	dispatchWorkerAndWait(executor, request);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("self_info_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult,
	         QStringLiteral("Plugin Name|Runtime Author|Runtime Description|Runtime Script|lua|"
	                        "worlds/plugins/runtime_plugin.xml|Plugin.Id|Runtime Purpose|/tmp/plugin/"));
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::emptyPluginVariableIdReadsWorldVariables()
{
	WorldRuntime runtime;
	runtime.setVariable(QStringLiteral("hour_offset"), QStringLiteral("1"));
	runtime.setVariable(QStringLiteral("MixedCaseMode"), QStringLiteral("active"));

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function OnPluginEnable()
  local hour_offset = GetPluginVariable("", "hour_offset")
  local mixed_case_mode = GetPluginVariable("", "mixedcasemode")
  empty_plugin_variable_ok = hour_offset == "1"
  empty_plugin_variable_case_ok = mixed_case_mode == "active"

  local variables = GetPluginVariableList("")
  empty_plugin_variable_list_ok = variables ~= nil and
                                  variables.hour_offset == "1" and
                                  variables.MixedCaseMode == "active"
  empty_plugin_variable_status = table.concat({
    tostring(hour_offset),
    tostring(mixed_case_mode),
    tostring(variables ~= nil),
    variables ~= nil and tostring(variables.hour_offset) or "no-table",
    variables ~= nil and tostring(variables.MixedCaseMode) or "no-table"
  }, "|")
  return empty_plugin_variable_ok and
         empty_plugin_variable_case_ok and
         empty_plugin_variable_list_ok
end
function empty_plugin_variable_status_value(value)
  return table.concat({
    tostring(empty_plugin_variable_ok),
    tostring(empty_plugin_variable_case_ok),
    tostring(empty_plugin_variable_list_ok),
    empty_plugin_variable_status or "no-status"
  }, "|")
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = captureVariableDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(result.boolResultValid);

	LuaBatchDispatchRequest statusRequest = request;
	statusRequest.kind                    = LuaBatchDispatchKind::StringInOut;
	statusRequest.functionName            = QStringLiteral("empty_plugin_variable_status_value");
	statusRequest.stringArg               = QStringLiteral("ignored");
	statusRequest.miniWindowSnapshotArg   = {};
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, statusRequest, statusResult);
	QVERIFY2(result.boolResult, qPrintable(statusResult.stringResult));
	QCOMPARE(statusResult.stringResult, QStringLiteral("true|true|true|1|active|true|1|active"));
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::deleteVariableInvalidatesCallbackVariableSnapshot()
{
	WorldRuntime runtime;
	runtime.setVariable(QStringLiteral("stale"), QStringLiteral("present"));

	const QSharedPointer<const LuaCallbackMiniWindowSnapshot> before =
	    captureVariableDispatchSnapshotForTest(runtime);
	QVERIFY(before);
	QVERIFY(before->hasWorldVariablesSnapshot);
	QCOMPARE(before->worldVariablesSnapshot.value(QStringLiteral("stale")), QStringLiteral("present"));

	QCOMPARE(runtime.deleteVariable(QStringLiteral("stale")), eOK);

	const QSharedPointer<const LuaCallbackMiniWindowSnapshot> after =
	    captureVariableDispatchSnapshotForTest(runtime);
	QVERIFY(after);
	QVERIFY(after->hasWorldVariablesSnapshot);
	QVERIFY(!after->worldVariablesSnapshot.contains(QStringLiteral("stale")));
}

void tst_LuaCallbackEngine::nativeShimDiscoveryIsAvailableWithoutShadowPlugin()
{
	WorldRuntime  runtime;
	QTemporaryDir soundRoot;
	QVERIFY(soundRoot.isValid());
	QVERIFY(QDir(soundRoot.path()).mkpath(QStringLiteral("sounds")));
	QFile soundFile(QDir(soundRoot.path()).filePath(QStringLiteral("sounds/coin.wav")));
	QVERIFY(soundFile.open(QIODevice::WriteOnly));
	soundFile.write("RIFF");
	soundFile.close();
	runtime.setStartupDirectory(soundRoot.path());
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	const QString     shimId = QMudNativePluginRegistry::mushReaderPluginId();
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local id = "925cdd0331023d9f0b8f05a7"
	  local audio_id = "aedf0cb0be5bf045860d54b7"
	  audio.volume(75)
	  local delay_id = audio.playDelay("coin.wav", 10)
	  local delayed_playing = audio.isPlaying(delay_id)
	  audio.free()
	  shim_info = table.concat({
	    GetPluginInfo(id, 1) or "",
	    tostring(GetPluginInfo(id, 17) or false),
	    GetPluginInfo(audio_id, 1) or "",
	    tostring(GetPluginInfo(audio_id, 17) or false),
	    string.format("%.0f", audio.getVolume()),
	    tostring(delay_id),
	    tostring(delayed_playing)
	  }, "|")
	end
	function shim_info_status(value)
	  return shim_info
	end
	)lua"),
	                       &runtime);

	auto snapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->soundStatusByBuffer.insert(1, -2);
	snapshot->soundBufferReusableByBuffer.insert(1, true);
	snapshot->soundStatusByBuffer.insert(9, 1);
	snapshot->soundBufferReusableByBuffer.insert(9, false);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = snapshot;
	dispatchWorkerAndWait(executor, request);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("shim_info_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("MushReader|false|LuaAudio|true|100|1|false"));
	QVERIFY(runtime.pluginIdList().contains(shimId, Qt::CaseInsensitive));
	QVERIFY(
	    runtime.pluginIdList().contains(QMudNativePluginRegistry::luaAudioPluginId(), Qt::CaseInsensitive));
	QCOMPARE(QMudNativePluginRegistry::pluginSupports(shimId, QStringLiteral("say")), eOK);
	QCOMPARE(QMudNativePluginRegistry::pluginSupports(shimId, QStringLiteral("interrupt")), eOK);
	QCOMPARE(QMudNativePluginRegistry::pluginSupports(shimId, QStringLiteral("stop")), eOK);
	QCOMPARE(QMudNativePluginRegistry::pluginSupports(shimId, QStringLiteral("missing")), eNoSuchRoutine);
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::nativeLuaAudioSharedRuntimeStateCoversDirectAndCallPlugin()
{
	WorldRuntime  runtime;
	QTemporaryDir soundRoot;
	QVERIFY(soundRoot.isValid());
	QVERIFY(QDir(soundRoot.path()).mkpath(QStringLiteral("sounds")));
	QFile soundFile(QDir(soundRoot.path()).filePath(QStringLiteral("sounds/coin.wav")));
	QVERIFY(soundFile.open(QIODevice::WriteOnly));
	soundFile.write("RIFF");
	soundFile.close();
	runtime.setStartupDirectory(soundRoot.path());

	const QMudNativePluginRegistry::NativeCallResult nativeDelayed = QMudNativePluginRegistry::callRoutine(
	    &runtime, QMudNativePluginRegistry::luaAudioPluginId(), QStringLiteral("playDelay"),
	    {QStringLiteral("coin.wav"), 10.0, 0.0, 100.0});
	QCOMPARE(nativeDelayed.errorCode, eOK);
	QCOMPARE(nativeDelayed.returnValues.size(), 1);
	QCOMPARE(nativeDelayed.returnValues.constFirst().toInt(), 1);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  audio.volume(75)
	  local delay_id = audio.playDelay("coin.wav", 10)
	  audio.volume(25, delay_id)
	  local delayed_volume = audio.getVolume(delay_id)
	  audio.free()
	  lua_audio_shared_info = table.concat({
	    tostring(delay_id),
	    string.format("%.0f", delayed_volume),
	    string.format("%.0f", audio.getVolume())
	  }, "|")
	end
	function lua_audio_shared_status(value)
	  return lua_audio_shared_info
	end
	)lua"),
	                       &runtime);

	auto snapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->soundStatusByBuffer.insert(1, -2);
	snapshot->soundBufferReusableByBuffer.insert(1, true);
	snapshot->soundStatusByBuffer.insert(2, -2);
	snapshot->soundBufferReusableByBuffer.insert(2, true);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = snapshot;
	dispatchWorkerAndWait(executor, request);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("lua_audio_shared_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("2|25|100"));
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());

	auto preStartEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, preStartEngine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  prestart_id = audio.play("coin.wav")
	end
	function prestart_audio_status(value)
	  return tostring(prestart_id)
	end
	)lua"),
	                       &runtime);
	auto preStartSnapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	preStartSnapshot->soundStatusByBuffer.insert(1, 0);
	preStartSnapshot->soundBufferReusableByBuffer.insert(1, false);
	preStartSnapshot->soundStatusByBuffer.insert(2, -2);
	preStartSnapshot->soundBufferReusableByBuffer.insert(2, true);
	request.engines               = {preStartEngine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = preStartSnapshot;
	dispatchWorkerAndWait(executor, request);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("prestart_audio_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("2"));
	teardownWorkerEngine(executor, preStartEngine);
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());

	auto callPluginPreStartEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, callPluginPreStartEngine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local code, id = CallPlugin("aedf0cb0be5bf045860d54b7", "play", "coin.wav")
	  callplugin_prestart_info = tostring(code) .. "|" .. tostring(id)
	end
	function callplugin_prestart_audio_status(value)
	  return callplugin_prestart_info
	end
	)lua"),
	                       &runtime);
	auto callPluginPreStartSnapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	callPluginPreStartSnapshot->soundStatusByBuffer.insert(1, 0);
	callPluginPreStartSnapshot->soundBufferReusableByBuffer.insert(1, false);
	callPluginPreStartSnapshot->soundStatusByBuffer.insert(2, -2);
	callPluginPreStartSnapshot->soundBufferReusableByBuffer.insert(2, true);
	request.engines               = {callPluginPreStartEngine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = callPluginPreStartSnapshot;
	dispatchWorkerAndWait(executor, request);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("callplugin_prestart_audio_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("0.0|2"));
	teardownWorkerEngine(executor, callPluginPreStartEngine);
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());

	auto callPluginStringBoolEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, callPluginStringBoolEngine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local code, id = CallPlugin("aedf0cb0be5bf045860d54b7", "play", "coin.wav", "1")
	  callplugin_string_bool_info = tostring(code) .. "|" .. tostring(id) .. "|" .. tostring(audio.isPlaying(id))
	end
	function callplugin_string_bool_status(value)
	  return callplugin_string_bool_info
	end
	)lua"),
	                       &runtime);
	auto callPluginStringBoolSnapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	callPluginStringBoolSnapshot->soundStatusByBuffer.insert(1, -2);
	callPluginStringBoolSnapshot->soundBufferReusableByBuffer.insert(1, true);
	request.engines               = {callPluginStringBoolEngine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = callPluginStringBoolSnapshot;
	dispatchWorkerAndWait(executor, request);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("callplugin_string_bool_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("0.0|1|true"));
	teardownWorkerEngine(executor, callPluginStringBoolEngine);
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());

	auto callPluginBadArgumentEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, callPluginBadArgumentEngine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local code, message = CallPlugin("aedf0cb0be5bf045860d54b7", "play", {})
	  callplugin_bad_argument_info = tostring(code == eBadParameter) .. "|" .. tostring(message)
	end
	function callplugin_bad_argument_status(value)
	  return callplugin_bad_argument_info
	end
	)lua"),
	                       &runtime);
	auto callPluginBadArgumentSnapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	callPluginBadArgumentSnapshot->soundStatusByBuffer.insert(1, -2);
	callPluginBadArgumentSnapshot->soundBufferReusableByBuffer.insert(1, true);
	request.engines               = {callPluginBadArgumentEngine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = callPluginBadArgumentSnapshot;
	dispatchWorkerAndWait(executor, request);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("callplugin_bad_argument_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("true|Cannot pass argument #3 (table type) to CallPlugin"));
	teardownWorkerEngine(executor, callPluginBadArgumentEngine);
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());

	auto callPluginBranchEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, callPluginBranchEngine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local audio_id = "aedf0cb0be5bf045860d54b7"
	  local delay_code, delayed_id = CallPlugin(audio_id, "playDelayLooped", "coin.wav", 10, 4, 60)
	  local set_code = CallPlugin(audio_id, "setVol", 44, 1)
	  local slide_vol_code = CallPlugin(audio_id, "slideVol", 35, 1, 0.02)
	  local slide_pan_code = CallPlugin(audio_id, "slidePan", 6, 1, 0.02)
	  local slide_pitch_code = CallPlugin(audio_id, "slidePitch", 7, 1, 0.02)
	  local fade_code = CallPlugin(audio_id, "fadeout", 2, 0.02)
	  local stop_code = CallPlugin(audio_id, "stop", 3)
	  local playing_code, stopped_playing = CallPlugin(audio_id, "isPlaying", 3)
	  local get_code, volume_before = CallPlugin(audio_id, "getVolume", 1)
	  callplugin_branch_info = table.concat({
	    tostring(delay_code),
	    tostring(delayed_id),
	    string.format("%.0f", volume_before),
	    tostring(set_code),
	    tostring(slide_vol_code),
	    tostring(slide_pan_code),
	    tostring(slide_pitch_code),
	    tostring(fade_code),
	    tostring(stop_code),
	    tostring(playing_code),
	    tostring(stopped_playing)
	  }, "|")
	end
	function callplugin_branch_status(value)
	  return callplugin_branch_info
	end
	)lua"),
	                       &runtime);
	QMudNativePluginRegistry::LuaAudioRuntimeBufferState callPluginBranchState;
	callPluginBranchState.volume   = 100.0;
	callPluginBranchState.ownerKey = callPluginBranchEngine.data();
	QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(&runtime, 1, callPluginBranchState);
	QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(&runtime, 2, callPluginBranchState);
	QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(&runtime, 3, callPluginBranchState);
	auto callPluginBranchSnapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	callPluginBranchSnapshot->soundStatusByBuffer.insert(1, 1);
	callPluginBranchSnapshot->soundBufferReusableByBuffer.insert(1, false);
	callPluginBranchSnapshot->soundStatusByBuffer.insert(2, 1);
	callPluginBranchSnapshot->soundBufferReusableByBuffer.insert(2, false);
	callPluginBranchSnapshot->soundStatusByBuffer.insert(3, 1);
	callPluginBranchSnapshot->soundBufferReusableByBuffer.insert(3, false);
	callPluginBranchSnapshot->soundStatusByBuffer.insert(4, -2);
	callPluginBranchSnapshot->soundBufferReusableByBuffer.insert(4, true);
	request.engines               = {callPluginBranchEngine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = callPluginBranchSnapshot;
	LuaBatchDispatchResult callPluginBranchResult;
	dispatchWorkerAndWait(executor, request, callPluginBranchResult);
	executeDeferredMutations(callPluginBranchResult);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("callplugin_branch_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("0.0|4|44|0.0|0.0|0.0|0.0|0.0|0.0|0.0|false"));
	QTRY_VERIFY_WITH_TIMEOUT(
	    (
	        [&]
	        {
		        if (!QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, callPluginBranchState))
			        return false;
		        QMudNativePluginRegistry::LuaAudioRuntimeBufferState stoppedState;
		        return callPluginBranchState.volume == 35.0 && callPluginBranchState.pan == 6.0 &&
		               callPluginBranchState.pitch == 7.0 &&
		               !QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 2, stoppedState) &&
		               !QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 3, stoppedState);
	        })(),
	    3000);
	teardownWorkerEngine(executor, callPluginBranchEngine);
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());

	teardownWorkerEngine(executor, engine);
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());

	auto pendingEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, pendingEngine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  audio.playDelay("coin.wav", 10)
	end
	)lua"),
	                       &runtime);
	auto pendingSnapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	pendingSnapshot->soundStatusByBuffer.insert(1, -2);
	pendingSnapshot->soundBufferReusableByBuffer.insert(1, true);
	request.engines               = {pendingEngine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = pendingSnapshot;
	dispatchWorkerAndWait(executor, request);
	QCOMPARE(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).size(), 1);

	teardownWorkerEngine(executor, pendingEngine);
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());

	auto timedEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, timedEngine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  timed_id = 1
	  fade_id = 2
	  audio.slideVol(40, timed_id, 0.02)
	  audio.slidePan(7, timed_id, 0.02)
	  audio.slidePitch(5, timed_id, 0.02)
	  audio.fadeout(fade_id, 0.02)
	  timed_before = audio.getVolume(timed_id)
	end
	function timed_audio_status(value)
	  return table.concat({
	    tostring(timed_id),
	    tostring(fade_id),
	    string.format("%.0f", timed_before),
	    string.format("%.0f", audio.getVolume(timed_id)),
	    tostring(audio.isPlaying(fade_id))
	  }, "|")
	end
	)lua"),
	                       &runtime);
	QMudNativePluginRegistry::LuaAudioRuntimeBufferState timedState;
	timedState.volume   = 100.0;
	timedState.ownerKey = timedEngine.data();
	QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(&runtime, 1, timedState);
	QMudNativePluginRegistry::LuaAudioRuntimeBufferState fadeState;
	fadeState.volume   = 100.0;
	fadeState.ownerKey = timedEngine.data();
	QMudNativePluginRegistry::luaAudioMarkRuntimeBuffer(&runtime, 2, fadeState);
	auto timedSnapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	timedSnapshot->soundStatusByBuffer.insert(1, 1);
	timedSnapshot->soundBufferReusableByBuffer.insert(1, false);
	timedSnapshot->soundStatusByBuffer.insert(2, 1);
	timedSnapshot->soundBufferReusableByBuffer.insert(2, false);
	request.engines               = {timedEngine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = timedSnapshot;
	dispatchWorkerAndWait(executor, request);

	QTest::qWait(60);
	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("timed_audio_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("1|2|100|40|false"));
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeBufferState(&runtime, 1, timedState));
	QCOMPARE(timedState.volume, 40.0);
	QCOMPARE(timedState.pan, 7.0);
	QCOMPARE(timedState.pitch, 5.0);
	QVERIFY(runtime.soundStatus(2) <= 0);
	teardownWorkerEngine(executor, timedEngine);
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
}

void tst_LuaCallbackEngine::blacklistedPluginsAreHiddenFromPluginApis()
{
	WorldRuntime      runtime;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	const QString     blacklistedId = QStringLiteral("bb6a05ed7534b5db1ed40511");
	const QStringList blacklistedIds{blacklistedId, QStringLiteral("b8e6dac1ee7fe8e3de931fb7"),
	                                 QStringLiteral("8238deec7c06bade8ebc3819")};
	for (const QString &id : blacklistedIds)
		QVERIFY(QMudNativePluginRegistry::isBlacklistedId(id));

	WorldRuntime::Plugin plugin;
	plugin.attributes.insert(QStringLiteral("id"), blacklistedId);
	plugin.attributes.insert(QStringLiteral("name"), QStringLiteral("Automatic Backup"));
	plugin.enabled = true;
	runtime.pluginsMutable().push_back(plugin);

	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local id = "bb6a05ed7534b5db1ed40511"
	  blacklist_status = tostring(GetPluginInfo(id, 1) == nil)
	end
	function blacklist_status_value(value)
	  return blacklist_status
	end
	)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("OnPluginEnable");
	dispatchWorkerAndWait(executor, request);

	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("blacklist_status_value");
	request.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("true"));
	QVERIFY(!runtime.pluginIdList().contains(blacklistedId, Qt::CaseInsensitive));
	QCOMPARE(QMudNativePluginRegistry::pluginSupports(blacklistedId, QStringLiteral("say")), eNoSuchPlugin);
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::triggerAnchoredColourOutputKeepsNativePromptText()
{
	WorldRuntime      runtime;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function prompt_cb(name, line)
  Tell("[")
  ColourTell("white", "black", "435")
  Tell(", ")
  ColourTell("white", "black", "1226")
  Note("]")
end
)lua"),
	                       &runtime);

	const QString           prompt = QStringLiteral("[Library][SAFE]<2084hp 1806sp 1695st> ");
	WorldRuntime::LineEntry promptEntry;
	promptEntry.text       = prompt;
	promptEntry.flags      = WorldRuntime::LineOutput;
	promptEntry.hardReturn = true;
	promptEntry.lineNumber = 42;
	runtime.replaceOutputLines({promptEntry});

	LuaBatchDispatchRequest request;
	request.engines        = {engine};
	request.kind           = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName   = QStringLiteral("prompt_cb");
	request.stringListArg  = {QStringLiteral("prompt"), prompt};
	request.stringListArg2 = {prompt};
	const auto styleRuns   = QSharedPointer<QVector<LuaStyleRun>>::create();
	styleRuns->push_back({prompt, 0xFFFFFF, 0x000000, 0});
	request.styleRunsArg                     = styleRuns;
	request.triggerMatchedLineBufferIndex    = 1;
	request.triggerMatchedLineAbsoluteNumber = promptEntry.lineNumber;
	request.triggerOutputReplacesMatchedLine = false;
	request.miniWindowSnapshotArg            = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(result.hasFunction);
	QVERIFY(!result.deferredRuntimeMutationBatches.isEmpty());
	executeDeferredMutations(result);

	const auto &lines = runtime.lines();
	QCOMPARE(logicalOutputLinesFromEntries(lines), QStringList({QStringLiteral("[435, 1226]"), prompt}));
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::stringsAndWildcardsDispatchSuppliesSnapshotForCallbackReads()
{
	WorldRuntime      runtime;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
	snapshot_seen = ""
	function timer_cb(name)
	  snapshot_seen = string.format("%dx%d|%d",
	    WindowInfo("map", 3) or -1,
	    WindowInfo("map", 4) or -1,
	    GetInfo(290))
	end
	function snapshot_status(value)
	  return snapshot_seen
	end
	)lua"),
	                       &runtime);

	auto snapshot                   = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasCommandUiSnapshot  = true;
	snapshot->commandUiHasFrameData = true;
	snapshot->commandUiValues[QStringLiteral("outputTextRectLeft")] = 19;
	snapshot->windowNames                                           = {QStringLiteral("map")};
	LuaCallbackMiniWindowSnapshot::WindowInfoSnapshot windowInfo;
	windowInfo.width                                    = 120;
	windowInfo.height                                   = 80;
	snapshot->windowInfoByWindow[QStringLiteral("map")] = windowInfo;
	snapshot->rebuildMiniWindowLookupCaches();

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("timer_cb");
	request.stringListArg         = {QStringLiteral("wait_timer")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(result.hasFunction);

	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("snapshot_status");
	request.stringArg    = QStringLiteral("ignored");
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("120x80|19"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::callbackSnapshotSuppliesGetInfoAndMiniWindowReads()
{
	WorldRuntime runtime;
	auto         engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
snapshot_seen = ""
function OnPluginEnable()
  snapshot_seen = string.format("%s|%dx%d|%d,%d|%s|%s|%d,%d,%d,%d",
    tostring(GetInfo(86)),
    WindowInfo("map", 3) or -1,
    WindowInfo("map", 4) or -1,
    WindowInfo("map", 14) or -1,
    WindowInfo("map", 15) or -1,
    table.concat(WindowList() or {}, ","),
    table.concat(WindowHotspotList("map") or {}, ","),
    GetInfo(290), GetInfo(291), GetInfo(292), GetInfo(293))
end
function snapshot_status(value)
  return snapshot_seen
end
)lua"));

	auto snapshot                   = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasCommandUiSnapshot  = true;
	snapshot->commandUiHasFrameData = true;
	snapshot->commandUiValues[QStringLiteral("selectedWord")]         = QStringLiteral("sextant");
	snapshot->commandUiValues[QStringLiteral("selectedWordResolved")] = true;
	snapshot->commandUiValues[QStringLiteral("outputTextRectLeft")]   = 19;
	snapshot->commandUiValues[QStringLiteral("outputTextRectTop")]    = 14;
	snapshot->commandUiValues[QStringLiteral("outputTextRectRight")]  = 318;
	snapshot->commandUiValues[QStringLiteral("outputTextRectBottom")] = 252;
	snapshot->windowNames.push_back(QStringLiteral("map"));
	LuaCallbackMiniWindowSnapshot::WindowInfoSnapshot windowInfo;
	windowInfo.width                                    = 120;
	windowInfo.height                                   = 80;
	windowInfo.lastMouseX                               = 33;
	windowInfo.lastMouseY                               = 44;
	snapshot->windowInfoByWindow[QStringLiteral("map")] = windowInfo;
	snapshot->hotspotIdsByWindow[QStringLiteral("map")] = {QStringLiteral("move")};
	snapshot->rebuildMiniWindowLookupCaches();

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = snapshot;
	static_cast<void>(executor.dispatchBatch(request));

	request.kind                        = LuaBatchDispatchKind::StringInOut;
	request.functionName                = QStringLiteral("snapshot_status");
	request.stringArg                   = QStringLiteral("ignored");
	const LuaBatchDispatchResult result = executor.dispatchBatch(request);
	QCOMPARE(result.stringResult, QStringLiteral("sextant|120x80|33,44|map|move|19,14,318,252"));
}

void tst_LuaCallbackEngine::miniWindowDragReleaseSeesResizedCallbackState()
{
	WorldRuntime runtime;
	auto         engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
events = {}
function OnResizeMove(flags, hotspot_id)
  table.insert(events, string.format("move:%d,%d:%d,%d:%d,%d",
    GetInfo(283), GetInfo(284), WindowInfo("win", 14), WindowInfo("win", 15),
    WindowInfo("win", 17), WindowInfo("win", 18)))
  WindowResize("win", 240, 160, 0)
  return false
end
function OnResizeRelease(flags, hotspot_id)
  table.insert(events, string.format("release:%d,%d:%d,%d",
    WindowInfo("win", 3), WindowInfo("win", 4), GetInfo(283), GetInfo(284)))
  return false
end
function resize_status(value)
  return table.concat(events, "|")
end
)lua"));

	QVERIFY(runtime.windowCreate(QStringLiteral("win"), 20, 30, 100, 80, 4, 0, QColor(), QString()) == 0);
	QVERIFY(runtime.windowAddHotspot(QStringLiteral("win"), QStringLiteral("resizer"), 88, 68, 100, 80,
	                                 QString(), QString(), QStringLiteral("OnResizeDown"), QString(),
	                                 QString(), QString(), 0, 0, QString()) == 0);

	const auto makeSnapshot = [&runtime](const int mouseX, const int mouseY)
	{
		const QString windowId = QStringLiteral("win");
		const int     left     = runtime.windowInfo(windowId, 1).toInt();
		const int     top      = runtime.windowInfo(windowId, 2).toInt();
		const int     width    = runtime.windowInfo(windowId, 3).toInt();
		const int     height   = runtime.windowInfo(windowId, 4).toInt();

		auto          snapshot                = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
		snapshot->hasCommandUiSnapshot        = true;
		snapshot->commandUiHasView            = true;
		snapshot->commandUiHasFrameData       = true;
		snapshot->commandUiOutputClientWidth  = 640;
		snapshot->commandUiOutputClientHeight = 480;
		snapshot->commandUiValues.insert(QStringLiteral("hasView"), true);
		snapshot->commandUiValues.insert(QStringLiteral("hasFrameData"), true);
		snapshot->commandUiValues.insert(QStringLiteral("hasLastMousePosition"), true);
		snapshot->commandUiValues.insert(QStringLiteral("lastMouseX"), mouseX);
		snapshot->commandUiValues.insert(QStringLiteral("lastMouseY"), mouseY);
		snapshot->commandUiValues.insert(QStringLiteral("outputClientWidth"), 640);
		snapshot->commandUiValues.insert(QStringLiteral("outputClientHeight"), 480);

		snapshot->windowNames.push_back(windowId);
		LuaCallbackMiniWindowSnapshot::WindowInfoSnapshot windowInfo;
		windowInfo.locationX                   = left;
		windowInfo.locationY                   = top;
		windowInfo.width                       = width;
		windowInfo.height                      = height;
		windowInfo.position                    = runtime.windowInfo(windowId, 7).toInt();
		windowInfo.flags                       = runtime.windowInfo(windowId, 8).toInt();
		windowInfo.rectLeft                    = left;
		windowInfo.rectTop                     = top;
		windowInfo.rectRight                   = left + width;
		windowInfo.rectBottom                  = top + height;
		windowInfo.lastMouseX                  = mouseX - left;
		windowInfo.lastMouseY                  = mouseY - top;
		windowInfo.clientMouseX                = mouseX;
		windowInfo.clientMouseY                = mouseY;
		windowInfo.mouseDownHotspot            = QStringLiteral("resizer");
		snapshot->windowInfoByWindow[windowId] = windowInfo;
		snapshot->hotspotIdsByWindow[windowId] = {QStringLiteral("resizer")};
		snapshot->rebuildMiniWindowLookupCaches();
		return snapshot;
	};

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines                   = {engine};
	request.kind                      = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.numberArg1                = 0;
	request.stringArg2                = QStringLiteral("resizer");
	request.functionName              = QStringLiteral("OnResizeMove");
	request.miniWindowSnapshotArg     = makeSnapshot(145, 165);
	LuaBatchDispatchResult moveResult = executor.dispatchBatch(request);
	QVERIFY(moveResult.boolResultValid);
	QVERIFY(!moveResult.boolResult);
	executeDeferredMutations(moveResult);
	QCOMPARE(runtime.windowInfo(QStringLiteral("win"), 3).toInt(), 240);
	QCOMPARE(runtime.windowInfo(QStringLiteral("win"), 4).toInt(), 160);

	request.functionName                 = QStringLiteral("OnResizeRelease");
	request.miniWindowSnapshotArg        = makeSnapshot(145, 165);
	LuaBatchDispatchResult releaseResult = executor.dispatchBatch(request);
	QVERIFY(releaseResult.boolResultValid);
	QVERIFY(!releaseResult.boolResult);

	request.kind                        = LuaBatchDispatchKind::StringInOut;
	request.functionName                = QStringLiteral("resize_status");
	request.stringArg                   = QStringLiteral("ignored");
	request.miniWindowSnapshotArg       = {};
	const LuaBatchDispatchResult result = executor.dispatchBatch(request);
	QCOMPARE(result.stringResult, QStringLiteral("move:145,165:125,135:145,165|release:240,160:145,165"));
}

void tst_LuaCallbackEngine::deferredRuntimeMutationSkipsDestroyedRuntime()
{
	auto              runtime = std::make_unique<WorldRuntime>();
	auto              engine  = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor;
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function OnPluginEnable()
  SaveState()
end
)lua"),
	                       runtime.get());

	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("OnPluginEnable");
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);

	QVERIFY(!result.deferredRuntimeMutationBatches.isEmpty());
	runtime.reset();
	for (const LuaDeferredRuntimeMutationBatch &batch : result.deferredRuntimeMutationBatches)
	{
		for (const std::function<void()> &mutation : batch.mutations)
			mutation();
	}
	teardownWorkerEngine(executor, engine);
}
// NOLINTEND(readability-convert-member-functions-to-static)

QTEST_GUILESS_MAIN(tst_LuaCallbackEngine)

#include "tst_LuaCallbackEngine.moc"
