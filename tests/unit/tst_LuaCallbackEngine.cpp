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
#include "MainFrame.h"
#include "MiniWindow.h"
#include "NativePluginRegistry.h"
#include "TestSoundData.h"
#include "WorldChildWindow.h"
#include "WorldCommandProcessor.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "helpers/LuaCallbackNotepadPresentationUtils.h"
#include "helpers/LuaExecutionUtils.h"
#include "helpers/MiniWindowUtils.h"
#include "helpers/PluginPathUtils.h"
#include "scripting/ScriptingErrors.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QCursor>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSet>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QTest>

#include <atomic>
#include <clocale>
#include <limits>
#include <memory>
#include <stdexcept>

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace
{
	LuaDeferredRuntimeMutationConsumer recoveredMutationConsumerForTest()
	{
		return [](QVector<LuaDeferredRuntimeMutationBatch> batches)
		{ QMudLuaDeferredRuntimeMutation::apply(std::move(batches)); };
	}

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
			void modalYieldResumeFailurePreservesCallbackDefaults();
			void modalYieldResumeSupportsStackedModalCalls();
			void partialDispatchFallbackPreservesAggregateResults();
			void workerModalResumeDefersPostModalRuntimeMutations();
			void modalYieldCancelPreventsCallbackContinuation();
			void callbackCatalogObserverTracksFunctionPresence();
			void directDestructionDoesNotNotifyCallbackCatalogObserver();
			void directDestructionDoesNotWaitForForeignRuntimeCleanup();
			void workerOwnsInitializedEngineUntilWorkerTeardown();
			void workerShutdownReturnsCleanupBatchesToOwnerThread();
			void workerShutdownRecoversUndeliveredTeardownBatches();
			void workerShutdownRecoveryExcludesConcurrentDelivery();
			void workerReentrantShutdownFromDeliveredMutationDoesNotDeadlock();
			void workerMutationCompletionsPreserveDispatchOrderAcrossTargets();
			void workerNullTargetMutationCompletionPreservesDispatchOrder();
			void workerThrowingMutationCompletionRecoversAndAdvances();
			void workerShutdownTeardownPreservesRetainedEngineOrder();
			void workerShutdownCompletesActiveRuntimeBridgeBeforeTeardown();
			void lineStyleSnapshotRoundTripPreservesColourState();
			void packageRestrictionsAreAppliedToExistingState();
			void worldLuaFileApisAcceptMixedSeparators();
			void worldLuaFileApisUseRuntimeHomeAcrossThreadAffinity();
			void worldLuaFileApisIgnoreProcessQmudHome();
			void luaVisiblePathApisReturnRelativePosix();
			void utilsMultiListBoxAcceptsMushclientArgumentOrder();
			void deferredRuntimeMutationBatchesPreserveOrderAndOwnership();
			void directExecutorDispatchesRealEngines();
			void setOptionUpdatesOnlyTabCompletionSymbolBehaviors();
			void callPluginMarshallingUsesTargetEngineState();
			void noArgsDispatchReportsCallbackFailure();
			void workerDispatchesPluginLifecycleCallbacksOnRealEngines();
			void workerSingleRecipientDispatchesDrainDeferredMutations();
			void workerSqliteResourcesOutliveCreatingCallbackCoroutine();
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
			void nativeShimDiscoveryRespectsShadowPluginVisibility();
			void nativeMushReaderEnableByNameUpdatesResolvedCallbackMetadata();
			void nativeMushReaderCallPluginUsesCallbackSpeechSnapshot();
			void nativeMushReaderCallPluginDefersSpeechToRuntimeThread();
			void nativeMushReaderDeferredCallPluginUsesRuntimeSpeechState();
			void nativeLuaAudioSharedRuntimeStateCoversDirectAndCallPlugin();
			void disabledNativeLuaAudioShadowBlocksCallbackCallPluginFastPath();
			void blacklistedPluginsAreHiddenFromPluginApis();
			void triggerAnchoredColourOutputKeepsNativePromptText();
			void triggerSnapshotPreservesPresentationCountAndIndexes();
			void triggerSnapshotPreservesMatchedMetadataAndRecentPresentation();
			void runtimeTriggerDispatchRepairsStalePresentationIndex();
			void stringsAndWildcardsDispatchSuppliesSnapshotForCallbackReads();
			void linePageBaselineCapturesOnlyLastPresentedLine();
			void workerGetLineInfoFetchesBoundedPresentationPages();
			void workerLinePageRefreshesPresentationCount();
			void workerLinePageRefreshesAfterCallbackOutputMutation();
			void workerDirtyLinePageDoesNotClampToStaleCount();
			void workerPresentationCountConsumersRefreshAfterMultilineOutput();
			void workerPresentationSnapshotsRefreshFrameDataCoherently();
			void numberAndStringResumeKeepsPresentationFlags();
			void workerExtremeLineNumbersDoNotOverflowPageBounds();
			void workerBookmarkUpdatesCachedLineState();
			void workerEmptyDeleteLinesDoesNotRefreshPresentation();
			void outputRemovalReconcilesActiveIncomingLineIdentity();
			void bufferedReplacementPageUsesLiveRuntimeEntry();
			void unusedHiddenReplacementAnchorIsRemovedAtContextEnd();
			void anchoredOutputPositioningAvoidsRepeatedFullScans();
			void anchoredOutputCursorSurvivesHeadEvictionWithoutRescan();
			void anchoredInsertionCursorFollowsStableLineIdentity();
			void workerAcceptedDeletionPageDoesNotRestoreActiveLine();
			void workerDeletedAnchoredOutputDoesNotAdvanceInsertionCursor();
			void workerEmptyTellDoesNotShiftAnchoredOutput();
			void workerDeferredOutputDeletionReconcilesPresentation();
			void workerOutputAfterDeletionDoesNotUseDeletedAnchor();
			void workerAnsiNoteTerminatesAndPreservesUtf8();
			void workerNoteHrTerminatesOpenOutputLine();
			void workerScreendrawReceivesCompletedPresentedLines();
			void workerDrawOutputWindowOutputDoesNotQueueRecursiveCallback();
			void workerAnchoredOutputWrapsFromOpenLineColumn();
			void workerAnchoredOutputCommitsBeforeScreendrawMutation();
			void workerNestedDispatchRefreshesDirtyLinePresentation();
			void workerNestedPageWithoutRecentLinesClearsCallerSnapshot();
			void workerEmptyBufferMultilineOutputRefreshesPresentation();
			void workerActiveIncomingLineOutputRefreshKeepsAppendedLines();
			void workerDirtyOutputSkipsUnusedNestedLinePage();
			void workerAnchoredOutputRefreshesAfterSuspendedBufferGrowth();
			void workerCallPluginPropagatesTargetLinePageSuspension();
			void workerNestedCrossWorldPageKeepsCallerPresentation();
			void workerCallPluginCancellationReleasesTarget();
			void workerBroadcastCancellationReleasesTarget();
			void workerResetCancelsSuspendedSelfCallPluginSafely();
			void workerResetCancelsSuspendedCallPluginTarget();
			void directDestructionCancelsSuspendedCallPluginTarget();
			void workerWorldProxyPagesTargetAndRestoresCallerPresentation();
			void workerVanishedRuntimeDoesNotPublishEmptyLinePage();
			void notepadMutationReplayKeepsCreateClaimsAttachedAcrossErase();
			void freshNotepadSnapshotDoesNotReplayFlushedClose();
			void workerNotepadCachesPreserveGlobalAndOwnerLists();
			void workerUnavailableNotepadRefreshDoesNotExportStaleCaches();
			void callbackSnapshotSuppliesGetInfoAndMiniWindowReads();
			void callbackMiniWindowResourceIdentityIsExact();
			void callbackMiniWindowStructuredCachesKeepDelimiterDistinctKeys();
			void miniWindowDragReleaseSeesResizedCallbackState();
			void absoluteMiniWindowBoundsRemainConsistentInsideCallback();
			void deferredRuntimeMutationSkipsDestroyedRuntime();
			// NOLINTEND(readability-convert-member-functions-to-static)
	};

	struct ActiveWorkerBridgeContext
	{
			QObject         *target{nullptr};
			QSemaphore       entered;
			std::atomic_bool bridgeRan{false};
	};

	int activeWorkerBridge(lua_State *state)
	{
		auto *context = static_cast<ActiveWorkerBridgeContext *>(lua_touserdata(state, lua_upvalueindex(1)));
		if (!context || !context->target)
		{
			lua_pushboolean(state, 0);
			return 1;
		}
		context->entered.release();
		const bool bridged = qmudLuaBridgeInvokeOnObjectThread(
		    context->target, [context] { context->bridgeRan.store(true, std::memory_order_release); });
		lua_pushboolean(state, bridged ? 1 : 0);
		return 1;
	}

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

	void
	seedPluginMetadataDispatchSnapshotForTest(const QSharedPointer<LuaCallbackMiniWindowSnapshot> &snapshot,
	                                          const WorldRuntime                                  &runtime)
	{
		Q_ASSERT(snapshot);
		if (!snapshot)
			return;
		snapshot->pluginIdsSnapshot = runtime.pluginIdList();
		for (const QString &pluginId : snapshot->pluginIdsSnapshot)
		{
			const QString key = pluginId.trimmed().toLower();
			if (key.isEmpty())
				continue;
			const QString pluginName = runtime.pluginInfo(key, 1).toString();
			snapshot->pluginIdsByLookupKey.insert(key, key);
			if (!pluginName.trimmed().isEmpty())
				snapshot->pluginIdsByLookupKey.insert(pluginName.trimmed().toLower(), key);
			snapshot->pluginNamesById.insert(key, pluginName);
			snapshot->pluginDirectoriesById.insert(key, runtime.pluginInfo(key, 20).toString());
			snapshot->pluginEnabledById.insert(key, runtime.pluginInfo(key, 17).toBool());
			if (key.compare(QMudNativePluginRegistry::mushReaderPluginId(), Qt::CaseInsensitive) == 0)
			{
				snapshot->nativePluginSpeechEnabledById.insert(
				    key, QMudNativePluginRegistry::isMushReaderSpeechEnabled(&runtime));
			}
			for (int infoType = 1; infoType <= 25; ++infoType)
				snapshot->pluginInfoValuesById[key].insert(infoType, runtime.pluginInfo(key, infoType));
		}
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
		snapshot->hasRecentLinesSnapshot               = true;
		snapshot->recentLinesSnapshot                  = runtime.recentLines();
		quint64                             generation = 0;
		QHash<int, WorldRuntime::LineEntry> ignoredEntries;
		QStringList                         ignoredRecentLines;
		const int                           lineCount =
		    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
		auto lineSnapshot                  = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
		lineSnapshot->lineBufferGeneration = generation;
		lineSnapshot->lineBufferCount      = lineCount;
		snapshot->lineBufferCount          = lineCount;
		snapshot->lineBufferSnapshot       = lineSnapshot;
		snapshot->hasLineBufferSnapshot    = true;
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
	                            WorldRuntime  *runtime  = nullptr,
	                            const QString &pluginId = QStringLiteral("Plugin.Id"))
	{
		QVERIFY(engine);
		LuaEngineObservedInitializationRequest initRequest;
		initRequest.engine              = engine.data();
		initRequest.workerLifetimeOwner = engine;
		initRequest.runtime             = runtime;
		initRequest.scriptText          = script;
		initRequest.pluginId            = pluginId;
		initRequest.pluginName          = QStringLiteral("Plugin Name");
		initRequest.pluginDirectory     = QStringLiteral("/tmp/plugin");

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
		QVector<LuaDeferredRuntimeMutationBatch> batches;
		batches.swap(result.deferredRuntimeMutationBatches);
		const auto execute = [&batches]
		{
			for (LuaDeferredRuntimeMutationBatch &batch : batches)
			{
				for (std::function<void()> &mutation : batch.mutations)
					mutation();
			}
			batches.clear();
		};
		if (result.deferredRuntimeMutationDelivery)
		{
			QVector<LuaDeferredRuntimeMutationBatch> currentBatches = std::move(batches);
			static_cast<void>(result.deferredRuntimeMutationDelivery->consumeForDelivery(
			    [currentBatches = std::move(currentBatches)](
			        QVector<LuaDeferredRuntimeMutationBatch> earlierBatches) mutable
			    {
				    earlierBatches += std::move(currentBatches);
				    for (LuaDeferredRuntimeMutationBatch &batch : earlierBatches)
				    {
					    for (std::function<void()> &mutation : batch.mutations)
					    {
						    if (mutation)
							    mutation();
					    }
				    }
			    }));
			result.deferredRuntimeMutationDelivery.clear();
			return;
		}
		execute();
	}

	bool completeWorkerSuspensions(const LuaExecutorWorker                 &executor,
	                               const QSharedPointer<LuaCallbackEngine> &engine, WorldRuntime &runtime,
	                               LuaBatchDispatchResult &result, int &resumeCount,
	                               const int maximumResumeCount = 8)
	{
		resumeCount = 0;
		while (result.suspended)
		{
			if (++resumeCount > maximumResumeCount || !result.hasPendingModalStringRequest)
				return false;
			executeDeferredMutations(result);
			QCoreApplication::processEvents();
			QString resumeResult;
			if (result.pendingModalStringRequest.guiCallable)
				resumeResult = result.pendingModalStringRequest.guiCallable();
			if (result.pendingModalStringRequest.beforeRuntimeResumeCallback)
			{
				result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, resumeResult);
			}
			LuaBatchDispatchRequest resume;
			resume.engines       = {engine};
			resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
			resume.modalResumeId = result.modalResumeId;
			resume.stringArg     = resumeResult;
			dispatchWorkerAndWait(executor, resume, result);
		}
		return true;
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
                 wildcards.named .. "|" .. tostring(wildcards.id == "") .. "|" ..
                 styles[1].text .. "|" ..
                 tostring(styles[2].style)
end
)lua"));

	QVector<LuaStyleRun> styleRuns;
	styleRuns.push_back({QStringLiteral("room"), 10, 11, 1});
	styleRuns.push_back({QStringLiteral(" exits"), 12, 13, 4});
	const QStringList            args{QStringLiteral("line"), QStringLiteral("match")};
	const QStringList            wildcards{QStringLiteral("whole"), QStringLiteral("capture")};
	const QMap<QString, QString> namedWildcards{
	    {QStringLiteral("id"),    QStringLiteral("")     },
        {QStringLiteral("named"), QStringLiteral("value")}
    };

	bool hasFunction = false;
	QVERIFY(engine.callFunctionWithStringsAndWildcards(QStringLiteral("trigger_cb"), args, wildcards,
	                                                   namedWildcards, &styleRuns, nullptr, &hasFunction, 6,
	                                                   true, 12, 345));
	QVERIFY(hasFunction);
	QCOMPARE(luaGlobalString(engine.luaState(), "trigger_seen"),
	         QStringLiteral("line|match|whole|value|true|room|4.0"));

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

void tst_LuaCallbackEngine::modalYieldResumeFailurePreservesCallbackDefaults()
{
	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("script_errors_to_output_window"), QStringLiteral("1"));
	QStringList reportedErrors;
	QObject::connect(&runtime, &WorldRuntime::outputRequested, &runtime,
	                 [&reportedErrors](const QString &text, const bool, const bool)
	                 { reportedErrors.push_back(text); });
	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
function FailBool(flags, hotspot)
  PickColour(-1)
  error("bool failure after resume")
end

function FailString(value)
  utils.inputbox("choose", "title", "")
  error("string failure after resume")
end

function FailBytes(value)
  utils.inputbox("choose", "title", "")
  error("bytes failure after resume")
end
)lua"));

	LuaExecutorDirect executor;
	auto              resume = [&](const LuaBatchDispatchResult &initialResult, const QString &resumeValue)
	{
		LuaBatchDispatchRequest resumeRequest;
		resumeRequest.engines       = {engine};
		resumeRequest.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resumeRequest.modalResumeId = initialResult.modalResumeId;
		resumeRequest.stringArg     = resumeValue;
		return executor.dispatchBatch(resumeRequest);
	};

	LuaBatchDispatchRequest boolRequest;
	boolRequest.engines                            = {engine};
	boolRequest.kind                               = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	boolRequest.functionName                       = QStringLiteral("FailBool");
	boolRequest.numberArg1                         = 1;
	boolRequest.stringArg2                         = QStringLiteral("hotspot");
	boolRequest.defaultResult                      = false;
	const LuaBatchDispatchResult initialBoolResult = executor.dispatchBatch(boolRequest);
	QVERIFY(initialBoolResult.suspended);
	const LuaBatchDispatchResult boolResult = resume(initialBoolResult, QStringLiteral("255"));
	QVERIFY(boolResult.boolResultValid);
	QVERIFY(!boolResult.boolResult);
	QVERIFY(boolResult.hasFunctionValid);
	QVERIFY(boolResult.hasFunction);

	LuaBatchDispatchRequest stringRequest;
	stringRequest.engines                            = {engine};
	stringRequest.kind                               = LuaBatchDispatchKind::StringInOut;
	stringRequest.functionName                       = QStringLiteral("FailString");
	stringRequest.stringArg                          = QStringLiteral("preserved string");
	const LuaBatchDispatchResult initialStringResult = executor.dispatchBatch(stringRequest);
	QVERIFY(initialStringResult.suspended);
	const LuaBatchDispatchResult stringResult =
	    resume(initialStringResult, acceptedModalStringResult(QStringLiteral("ignored")));
	QCOMPARE(stringResult.stringResult, QStringLiteral("preserved string"));

	LuaBatchDispatchRequest bytesRequest;
	bytesRequest.engines                            = {engine};
	bytesRequest.kind                               = LuaBatchDispatchKind::BytesInOut;
	bytesRequest.functionName                       = QStringLiteral("FailBytes");
	bytesRequest.bytesArg                           = QByteArray("preserved bytes");
	const LuaBatchDispatchResult initialBytesResult = executor.dispatchBatch(bytesRequest);
	QVERIFY(initialBytesResult.suspended);
	const LuaBatchDispatchResult bytesResult =
	    resume(initialBytesResult, acceptedModalStringResult(QStringLiteral("ignored")));
	QCOMPARE(bytesResult.bytesResult, QByteArray("preserved bytes"));
	QCOMPARE(reportedErrors.size(), 3);
	QVERIFY(reportedErrors.at(0).contains(QStringLiteral("bool failure after resume")));
	QVERIFY(reportedErrors.at(1).contains(QStringLiteral("string failure after resume")));
	QVERIFY(reportedErrors.at(2).contains(QStringLiteral("bytes failure after resume")));
}

void tst_LuaCallbackEngine::partialDispatchFallbackPreservesAggregateResults()
{
	LuaBatchDispatchResult partial;
	partial.boolResult                      = false;
	partial.hasFunction                     = true;
	partial.countResult                     = 2;
	partial.stringResult                    = QStringLiteral("transformed string");
	partial.bytesResult                     = QByteArray("transformed bytes");
	partial.linePresentationRequiresRefresh = true;

	LuaBatchDispatchResult noArgsFallback;
	preserveLuaBatchPartialResultOnFallback(LuaBatchDispatchKind::NoArgs, noArgsFallback, partial);
	QVERIFY(noArgsFallback.boolResultValid);
	QVERIFY(!noArgsFallback.boolResult);
	QVERIFY(noArgsFallback.hasFunctionValid);
	QVERIFY(noArgsFallback.hasFunction);
	QVERIFY(noArgsFallback.linePresentationRequiresRefresh);

	LuaBatchDispatchResult countFallback;
	preserveLuaBatchPartialResultOnFallback(LuaBatchDispatchKind::NumberAndUtf8StringsCount, countFallback,
	                                        partial);
	QVERIFY(countFallback.countResultValid);
	QCOMPARE(countFallback.countResult, 2);

	LuaBatchDispatchResult stringFallback;
	stringFallback.stringResult = QStringLiteral("original string");
	preserveLuaBatchPartialResultOnFallback(LuaBatchDispatchKind::StringInOut, stringFallback, partial);
	QCOMPARE(stringFallback.stringResult, QStringLiteral("transformed string"));

	LuaBatchDispatchResult bytesFallback;
	bytesFallback.bytesResult = QByteArray("original bytes");
	preserveLuaBatchPartialResultOnFallback(LuaBatchDispatchKind::BytesInOut, bytesFallback, partial);
	QCOMPARE(bytesFallback.bytesResult, QByteArray("transformed bytes"));
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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

void tst_LuaCallbackEngine::directDestructionDoesNotNotifyCallbackCatalogObserver()
{
	int  observerCalls = 0;
	auto engine        = std::make_unique<LuaCallbackEngine>();
	engine->setCallbackCatalogObserver([&](const QString &, const QSet<QString> &, const QSet<QString> &)
	                                   { ++observerCalls; });
	engine->setScriptText(QStringLiteral("function observed() end"));
	QVERIFY(engine->loadScript());
	const int callsBeforeDestruction = observerCalls;
	engine.reset();
	QCOMPARE(observerCalls, callsBeforeDestruction);
}

void tst_LuaCallbackEngine::directDestructionDoesNotWaitForForeignRuntimeCleanup()
{
	WorldRuntime runtime;
	QThread      runtimeThread;
	QSemaphore   runtimeBlocked;
	QSemaphore   unblockRuntime;
	runtime.moveToThread(&runtimeThread);
	runtimeThread.start();
	QThread *const testThread           = QThread::currentThread();
	const auto     cleanupRuntimeThread = qScopeGuard(
	    [&]
	    {
		    unblockRuntime.release();
		    if (runtimeThread.isRunning() && runtime.thread() == &runtimeThread)
		    {
			    static_cast<void>(QMetaObject::invokeMethod(
			        &runtime, [&runtime, testThread] { runtime.moveToThread(testThread); },
			        Qt::BlockingQueuedConnection));
		    }
		    runtimeThread.quit();
		    runtimeThread.wait();
	    });
	QVERIFY(QMetaObject::invokeMethod(
	    &runtime,
	    [&runtimeBlocked, &unblockRuntime]
	    {
		    runtimeBlocked.release();
		    unblockRuntime.acquire();
	    },
	    Qt::QueuedConnection));
	QVERIFY(runtimeBlocked.tryAcquire(1, 3000));

	std::atomic_bool               mutationApplied{false};
	auto                           engine = std::make_unique<LuaCallbackEngine>();
	QVector<std::function<void()>> mutations;
	mutations.push_back([&mutationApplied] { mutationApplied.store(true, std::memory_order_release); });
	engine->appendDeferredRuntimeMutationBatch(&runtime, std::move(mutations));

	QElapsedTimer destructionTimer;
	destructionTimer.start();
	engine.reset();
	QVERIFY(destructionTimer.elapsed() < 1000);
	QVERIFY(!mutationApplied.load(std::memory_order_acquire));

	unblockRuntime.release();
	QTRY_VERIFY_WITH_TIMEOUT(mutationApplied.load(std::memory_order_acquire), 3000);
	QVERIFY(QMetaObject::invokeMethod(
	    &runtime, [&runtime, testThread] { runtime.moveToThread(testThread); },
	    Qt::BlockingQueuedConnection));
}

void tst_LuaCallbackEngine::workerOwnsInitializedEngineUntilWorkerTeardown()
{
	QThread *destructionThread = nullptr;
	auto     executor          = std::make_unique<LuaExecutorWorker>(recoveredMutationConsumerForTest());
	auto     engine = QSharedPointer<LuaCallbackEngine>(new LuaCallbackEngine(),
	                                                    [&destructionThread](const LuaCallbackEngine *value)
	                                                    {
		                                                destructionThread = QThread::currentThread();
		                                                delete value;
	                                                    });
	initializeWorkerEngine(*executor, engine, QStringLiteral("function retained() return true end"));

	const QWeakPointer<LuaCallbackEngine> weakEngine = engine;
	engine.clear();
	QVERIFY(!weakEngine.isNull());
	executor.reset();

	QVERIFY(weakEngine.isNull());
	QVERIFY(destructionThread);
	QVERIFY(destructionThread != QThread::currentThread());
}

void tst_LuaCallbackEngine::workerShutdownReturnsCleanupBatchesToOwnerThread()
{
	WorldRuntime runtime;
	bool         consumerCalled  = false;
	bool         mutationApplied = false;
	QThread     *consumerThread  = nullptr;
	auto         executor        = std::make_unique<LuaExecutorWorker>(
	    [&](const QVector<LuaDeferredRuntimeMutationBatch> &batches)
	    {
		    consumerCalled = true;
		    consumerThread = QThread::currentThread();
		    for (const LuaDeferredRuntimeMutationBatch &batch : batches)
		    {
			    for (const std::function<void()> &mutation : batch.mutations)
			    {
				    if (mutation)
					    mutation();
			    }
		    }
	    });
	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(*executor, engine, QStringLiteral("function retained() return true end"),
	                       &runtime);

	std::atomic_bool        appended{false};
	LuaBatchDispatchRequest request;
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.engines      = {engine};
	request.functionName = QStringLiteral("retained");
	executor->dispatchBatchAsync(
	    request, nullptr,
	    [engine, &runtime, &appended, &mutationApplied](const LuaBatchDispatchResult &)
	    {
		    QVector<std::function<void()>> mutations;
		    mutations.push_back([&mutationApplied] { mutationApplied = true; });
		    engine->appendDeferredRuntimeMutationBatch(&runtime, std::move(mutations));
		    appended.store(true, std::memory_order_release);
	    });
	QTRY_VERIFY_WITH_TIMEOUT(appended.load(std::memory_order_acquire), 3000);

	request.engines.clear();
	engine.clear();
	executor.reset();
	QVERIFY(consumerCalled);
	QCOMPARE(consumerThread, QThread::currentThread());
	QVERIFY(mutationApplied);
}

void tst_LuaCallbackEngine::workerShutdownRecoversUndeliveredTeardownBatches()
{
	WorldRuntime runtime;
	QStringList  mutationOrder;
	auto         executor = std::make_unique<LuaExecutorWorker>(
	    [](const QVector<LuaDeferredRuntimeMutationBatch> &batches)
	    {
		    for (const LuaDeferredRuntimeMutationBatch &batch : batches)
		    {
			    for (const std::function<void()> &mutation : batch.mutations)
			    {
				    if (mutation)
					    mutation();
			    }
		    }
	    });
	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(*executor, engine, QStringLiteral("function retained() return true end"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.kind                      = LuaBatchDispatchKind::NoArgs;
	request.engines                   = {engine};
	request.functionName              = QStringLiteral("retained");
	const auto appendMutationOnWorker = [&](const QString &marker)
	{
		executor->dispatchBatchAsync(
		    request, nullptr,
		    [engine, &runtime, &mutationOrder, marker](const LuaBatchDispatchResult &)
		    {
			    QVector<std::function<void()>> mutations;
			    mutations.push_back([&mutationOrder, marker] { mutationOrder.push_back(marker); });
			    engine->appendDeferredRuntimeMutationBatch(&runtime, std::move(mutations));
		    });
	};

	QThread        completionThread;
	QObject        completionTarget;
	QThread *const testThread = QThread::currentThread();
	QSemaphore     completionBlocked;
	QSemaphore     releaseCompletion;
	completionTarget.moveToThread(&completionThread);
	completionThread.start();
	const auto stopCompletionThread = qScopeGuard(
	    [&]
	    {
		    releaseCompletion.release();
		    if (completionTarget.thread() == &completionThread && completionThread.isRunning())
		    {
			    static_cast<void>(
			        qmudLuaBridgeInvokeOnObjectThread(&completionTarget, [&completionTarget, testThread]
			                                          { completionTarget.moveToThread(testThread); }));
		    }
		    completionThread.quit();
		    static_cast<void>(completionThread.wait());
	    });
	QVERIFY(QMetaObject::invokeMethod(
	    &completionTarget,
	    [&]
	    {
		    completionBlocked.release();
		    releaseCompletion.acquire();
	    },
	    Qt::QueuedConnection));
	QVERIFY(completionBlocked.tryAcquire(1, 3000));
	std::atomic_int completionCount{0};
	const auto      queueUndeliveredResult = [&]
	{
		executor->dispatchBatchAsync(request, &completionTarget,
		                             [&completionCount](const LuaBatchDispatchResult &dispatchResult)
		                             {
			                             LuaBatchDispatchResult result = dispatchResult;
			                             QMudLuaDeferredRuntimeMutation::apply(result);
			                             completionCount.fetch_add(1, std::memory_order_release);
		                             });
	};

	appendMutationOnWorker(QStringLiteral("first"));
	queueUndeliveredResult();
	appendMutationOnWorker(QStringLiteral("second"));
	queueUndeliveredResult();
	appendMutationOnWorker(QStringLiteral("teardown"));

	LuaBatchDispatchRequest barrierRequest;
	barrierRequest.kind = LuaBatchDispatchKind::HasFunction;
	static_cast<void>(executor->dispatchBatch(barrierRequest));
	QCOMPARE(completionCount.load(std::memory_order_acquire), 0);
	QVERIFY(mutationOrder.isEmpty());

	request.engines.clear();
	engine.clear();
	executor.reset();
	QCOMPARE(mutationOrder,
	         QStringList({QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("teardown")}));
	releaseCompletion.release();
	QTRY_COMPARE_WITH_TIMEOUT(completionCount.load(std::memory_order_acquire), 2, 3000);
	QCOMPARE(mutationOrder,
	         QStringList({QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("teardown")}));
}

void tst_LuaCallbackEngine::workerShutdownRecoveryExcludesConcurrentDelivery()
{
	WorldRuntime runtime;
	QStringList  mutationOrder;
	QSemaphore   recoveryEntered;
	QSemaphore   allowRecovery;
	auto         executor = std::make_unique<LuaExecutorWorker>(
	    [&](QVector<LuaDeferredRuntimeMutationBatch> batches)
	    {
		    recoveryEntered.release();
		    allowRecovery.acquire();
		    QMudLuaDeferredRuntimeMutation::apply(std::move(batches));
	    });
	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(*executor, engine, QStringLiteral("function retained() return true end"),
	                       &runtime);

	QThread        foreignThread;
	QObject        foreignTarget;
	QThread *const testThread = QThread::currentThread();
	foreignTarget.moveToThread(&foreignThread);
	foreignThread.start();
	QSemaphore foreignBlocked;
	QSemaphore unblockForeign;
	const auto stopForeignThread = qScopeGuard(
	    [&]
	    {
		    unblockForeign.release();
		    if (foreignTarget.thread() == &foreignThread && foreignThread.isRunning())
		    {
			    static_cast<void>(
			        qmudLuaBridgeInvokeOnObjectThread(&foreignTarget, [&foreignTarget, testThread]
			                                          { foreignTarget.moveToThread(testThread); }));
		    }
		    foreignThread.quit();
		    static_cast<void>(foreignThread.wait());
	    });
	QVERIFY(QMetaObject::invokeMethod(
	    &foreignTarget,
	    [&]
	    {
		    foreignBlocked.release();
		    unblockForeign.acquire();
	    },
	    Qt::QueuedConnection));
	QVERIFY(foreignBlocked.tryAcquire(1, 3000));

	LuaBatchDispatchRequest request;
	request.kind                      = LuaBatchDispatchKind::NoArgs;
	request.engines                   = {engine};
	request.functionName              = QStringLiteral("retained");
	const auto appendMutationOnWorker = [&](const QString &marker)
	{
		executor->dispatchBatchAsync(
		    request, nullptr,
		    [engine, &runtime, &mutationOrder, marker](const LuaBatchDispatchResult &)
		    {
			    QVector<std::function<void()>> mutations;
			    mutations.push_back([&mutationOrder, marker] { mutationOrder.push_back(marker); });
			    engine->appendDeferredRuntimeMutationBatch(&runtime, std::move(mutations));
		    });
	};

	std::atomic_bool mainCompletionFinished{false};
	appendMutationOnWorker(QStringLiteral("first"));
	executor->dispatchBatchAsync(request, &foreignTarget,
	                             [&mainCompletionFinished](const LuaBatchDispatchResult &dispatchResult)
	                             {
		                             LuaBatchDispatchResult result = dispatchResult;
		                             QMudLuaDeferredRuntimeMutation::apply(result);
		                             mainCompletionFinished.store(true, std::memory_order_release);
	                             });

	QSemaphore       foreignDeliveryAttempted;
	std::atomic_bool foreignCompletionFinished{false};
	appendMutationOnWorker(QStringLiteral("second"));
	executor->dispatchBatchAsync(
	    request, &foreignTarget,
	    [&foreignDeliveryAttempted, &foreignCompletionFinished](const LuaBatchDispatchResult &dispatchResult)
	    {
		    foreignDeliveryAttempted.release();
		    LuaBatchDispatchResult result = dispatchResult;
		    QMudLuaDeferredRuntimeMutation::apply(result);
		    foreignCompletionFinished.store(true, std::memory_order_release);
	    });
	LuaBatchDispatchRequest barrierRequest;
	barrierRequest.kind = LuaBatchDispatchKind::HasFunction;
	static_cast<void>(executor->dispatchBatch(barrierRequest));
	request.engines.clear();
	engine.clear();

	QThread *shutdownThread =
	    QThread::create([ownedExecutor = std::move(executor)]() mutable { ownedExecutor.reset(); });
	shutdownThread->start();
	QVERIFY(recoveryEntered.tryAcquire(1, 3000));
	unblockForeign.release();
	QCoreApplication::processEvents();
	QVERIFY(foreignDeliveryAttempted.tryAcquire(1, 3000));
	QTRY_VERIFY_WITH_TIMEOUT(foreignCompletionFinished.load(std::memory_order_acquire), 3000);
	QVERIFY(mainCompletionFinished.load(std::memory_order_acquire));
	QVERIFY(mutationOrder.isEmpty());
	allowRecovery.release();
	QTRY_VERIFY_WITH_TIMEOUT(!shutdownThread->isRunning(), 3000);
	QVERIFY(shutdownThread->wait());
	delete shutdownThread;

	QCOMPARE(mutationOrder, QStringList({QStringLiteral("first"), QStringLiteral("second")}));
	QTRY_VERIFY_WITH_TIMEOUT(foreignCompletionFinished.load(std::memory_order_acquire), 3000);
	QTRY_VERIFY_WITH_TIMEOUT(mainCompletionFinished.load(std::memory_order_acquire), 3000);
	QCOMPARE(mutationOrder, QStringList({QStringLiteral("first"), QStringLiteral("second")}));
	QVERIFY(QMetaObject::invokeMethod(
	    &foreignTarget, [&foreignTarget, testThread] { foreignTarget.moveToThread(testThread); },
	    Qt::BlockingQueuedConnection));
	foreignThread.quit();
	QVERIFY(foreignThread.wait(3000));
}

void tst_LuaCallbackEngine::workerReentrantShutdownFromDeliveredMutationDoesNotDeadlock()
{
	WorldRuntime runtime;
	auto         executor = std::make_unique<LuaExecutorWorker>(recoveredMutationConsumerForTest());
	auto         engine   = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(*executor, engine, QStringLiteral("function retained() return true end"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.engines      = {engine};
	request.functionName = QStringLiteral("retained");
	std::atomic_bool mutationAppended{false};
	executor->dispatchBatchAsync(
	    request, nullptr,
	    [engine, &runtime, &executor, &mutationAppended](const LuaBatchDispatchResult &)
	    {
		    QVector<std::function<void()>> mutations;
		    mutations.push_back([&executor] { executor.reset(); });
		    engine->appendDeferredRuntimeMutationBatch(&runtime, std::move(mutations));
		    mutationAppended.store(true, std::memory_order_release);
	    });
	QTRY_VERIFY_WITH_TIMEOUT(mutationAppended.load(std::memory_order_acquire), 3000);

	std::atomic_bool completionReturned{false};
	QObject          completionTarget;
	executor->dispatchBatchAsync(request, &completionTarget,
	                             [&completionReturned](const LuaBatchDispatchResult &dispatchResult)
	                             {
		                             LuaBatchDispatchResult result = dispatchResult;
		                             QMudLuaDeferredRuntimeMutation::apply(result);
		                             completionReturned.store(true, std::memory_order_release);
	                             });
	QTRY_VERIFY_WITH_TIMEOUT(completionReturned.load(std::memory_order_acquire), 3000);
	QVERIFY(!executor);
}

void tst_LuaCallbackEngine::workerMutationCompletionsPreserveDispatchOrderAcrossTargets()
{
	WorldRuntime      runtime;
	QStringList       mutationOrder;
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, engine, QStringLiteral("function retained() return true end"), &runtime);

	QThread        firstThread;
	QThread        secondThread;
	QObject        firstTarget;
	QObject        secondTarget;
	QThread *const testThread = QThread::currentThread();
	firstTarget.moveToThread(&firstThread);
	secondTarget.moveToThread(&secondThread);
	firstThread.start();
	secondThread.start();
	QSemaphore firstTargetBlocked;
	QSemaphore releaseFirstTarget;
	QVERIFY(QMetaObject::invokeMethod(
	    &firstTarget,
	    [&]
	    {
		    firstTargetBlocked.release();
		    releaseFirstTarget.acquire();
	    },
	    Qt::QueuedConnection));
	QVERIFY(firstTargetBlocked.tryAcquire(1, 3000));

	LuaBatchDispatchRequest request;
	request.kind                      = LuaBatchDispatchKind::NoArgs;
	request.engines                   = {engine};
	request.functionName              = QStringLiteral("retained");
	const auto appendMutationOnWorker = [&](const QString &marker)
	{
		std::atomic_bool appended{false};
		executor.dispatchBatchAsync(
		    request, nullptr,
		    [engine, &runtime, &mutationOrder, &appended, marker](const LuaBatchDispatchResult &)
		    {
			    QVector<std::function<void()>> mutations;
			    mutations.push_back([&mutationOrder, marker] { mutationOrder.push_back(marker); });
			    engine->appendDeferredRuntimeMutationBatch(&runtime, std::move(mutations));
			    appended.store(true, std::memory_order_release);
		    });
		QTRY_VERIFY_WITH_TIMEOUT(appended.load(std::memory_order_acquire), 3000);
	};

	std::atomic_bool firstCompleted{false};
	std::atomic_bool secondCompleted{false};
	appendMutationOnWorker(QStringLiteral("first"));
	executor.dispatchBatchAsync(request, &firstTarget,
	                            [&](const LuaBatchDispatchResult &dispatchResult)
	                            {
		                            LuaBatchDispatchResult result = dispatchResult;
		                            QMudLuaDeferredRuntimeMutation::apply(result);
		                            firstCompleted.store(true, std::memory_order_release);
	                            });
	appendMutationOnWorker(QStringLiteral("second"));
	executor.dispatchBatchAsync(request, &secondTarget,
	                            [&](const LuaBatchDispatchResult &dispatchResult)
	                            {
		                            LuaBatchDispatchResult result = dispatchResult;
		                            QMudLuaDeferredRuntimeMutation::apply(result);
		                            secondCompleted.store(true, std::memory_order_release);
	                            });
	LuaBatchDispatchRequest barrierRequest;
	barrierRequest.kind = LuaBatchDispatchKind::HasFunction;
	static_cast<void>(executor.dispatchBatch(barrierRequest));
	QTest::qWait(50);
	QVERIFY(!firstCompleted.load(std::memory_order_acquire));
	QVERIFY(!secondCompleted.load(std::memory_order_acquire));

	releaseFirstTarget.release();
	QTRY_VERIFY_WITH_TIMEOUT(firstCompleted.load(std::memory_order_acquire), 3000);
	QTRY_VERIFY_WITH_TIMEOUT(secondCompleted.load(std::memory_order_acquire), 3000);
	QCOMPARE(mutationOrder, QStringList({QStringLiteral("first"), QStringLiteral("second")}));

	QVERIFY(QMetaObject::invokeMethod(
	    &firstTarget, [&firstTarget, testThread] { firstTarget.moveToThread(testThread); },
	    Qt::BlockingQueuedConnection));
	QVERIFY(QMetaObject::invokeMethod(
	    &secondTarget, [&secondTarget, testThread] { secondTarget.moveToThread(testThread); },
	    Qt::BlockingQueuedConnection));
	firstThread.quit();
	secondThread.quit();
	QVERIFY(firstThread.wait(3000));
	QVERIFY(secondThread.wait(3000));

	mutationOrder.clear();
	QThread           abandonedThread;
	auto             *abandonedTarget = new QObject();
	QPointer<QObject> abandonedGuard(abandonedTarget);
	abandonedTarget->moveToThread(&abandonedThread);
	abandonedThread.start();
	QSemaphore abandonedBlocked;
	QSemaphore releaseAbandoned;
	const auto stopAbandonedThread = qScopeGuard(
	    [&]
	    {
		    releaseAbandoned.release();
		    abandonedThread.quit();
		    static_cast<void>(abandonedThread.wait());
	    });
	QVERIFY(QMetaObject::invokeMethod(
	    abandonedTarget,
	    [&]
	    {
		    abandonedBlocked.release();
		    releaseAbandoned.acquire();
	    },
	    Qt::QueuedConnection));
	QVERIFY(abandonedBlocked.tryAcquire(1, 3000));
	QVERIFY(QMetaObject::invokeMethod(
	    abandonedTarget, [abandonedTarget] { delete abandonedTarget; }, Qt::QueuedConnection));
	std::atomic_bool abandonedCompletion{false};
	appendMutationOnWorker(QStringLiteral("abandoned"));
	executor.dispatchBatchAsync(request, abandonedTarget,
	                            [&](const LuaBatchDispatchResult &dispatchResult)
	                            {
		                            LuaBatchDispatchResult result = dispatchResult;
		                            QMudLuaDeferredRuntimeMutation::apply(result);
		                            abandonedCompletion.store(true, std::memory_order_release);
	                            });
	static_cast<void>(executor.dispatchBatch(barrierRequest));
	releaseAbandoned.release();
	QTRY_VERIFY_WITH_TIMEOUT(abandonedGuard.isNull(), 3000);
	QTRY_COMPARE_WITH_TIMEOUT(mutationOrder, QStringList({QStringLiteral("abandoned")}), 3000);
	QVERIFY(!abandonedCompletion.load(std::memory_order_acquire));
	abandonedThread.quit();
	QVERIFY(abandonedThread.wait(3000));

	QThread           stoppedThread;
	auto             *stoppedTarget = new QObject();
	QPointer<QObject> stoppedGuard(stoppedTarget);
	QSemaphore        stoppedBlocked;
	QSemaphore        releaseStopped;
	stoppedTarget->moveToThread(&stoppedThread);
	QObject::connect(&stoppedThread, &QThread::finished, stoppedTarget, &QObject::deleteLater);
	stoppedThread.start();
	const auto stopStoppedThread = qScopeGuard(
	    [&]
	    {
		    releaseStopped.release();
		    stoppedThread.quit();
		    static_cast<void>(stoppedThread.wait());
	    });
	QVERIFY(QMetaObject::invokeMethod(
	    stoppedTarget,
	    [&]
	    {
		    stoppedBlocked.release();
		    releaseStopped.acquire();
	    },
	    Qt::QueuedConnection));
	QVERIFY(stoppedBlocked.tryAcquire(1, 3000));
	std::atomic_bool stoppedCompletion{false};
	appendMutationOnWorker(QStringLiteral("stopped"));
	executor.dispatchBatchAsync(request, stoppedTarget,
	                            [&](const LuaBatchDispatchResult &dispatchResult)
	                            {
		                            LuaBatchDispatchResult result = dispatchResult;
		                            QMudLuaDeferredRuntimeMutation::apply(result);
		                            stoppedCompletion.store(true, std::memory_order_release);
	                            });
	static_cast<void>(executor.dispatchBatch(barrierRequest));
	stoppedThread.quit();
	releaseStopped.release();
	QVERIFY(stoppedThread.wait(3000));
	QTRY_COMPARE_WITH_TIMEOUT(mutationOrder,
	                          QStringList({QStringLiteral("abandoned"), QStringLiteral("stopped")}), 3000);
	QVERIFY(!stoppedCompletion.load(std::memory_order_acquire));
	QTRY_VERIFY_WITH_TIMEOUT(stoppedGuard.isNull(), 3000);

	QObject          followingTarget;
	std::atomic_bool followingCompleted{false};
	appendMutationOnWorker(QStringLiteral("following"));
	executor.dispatchBatchAsync(request, &followingTarget,
	                            [&](const LuaBatchDispatchResult &dispatchResult)
	                            {
		                            LuaBatchDispatchResult result = dispatchResult;
		                            QMudLuaDeferredRuntimeMutation::apply(result);
		                            followingCompleted.store(true, std::memory_order_release);
	                            });
	static_cast<void>(executor.dispatchBatch(barrierRequest));
	QTRY_VERIFY_WITH_TIMEOUT(followingCompleted.load(std::memory_order_acquire), 3000);
	QCOMPARE(mutationOrder, QStringList({QStringLiteral("abandoned"), QStringLiteral("stopped"),
	                                     QStringLiteral("following")}));
}

void tst_LuaCallbackEngine::workerNullTargetMutationCompletionPreservesDispatchOrder()
{
	WorldRuntime      runtime;
	QStringList       mutationOrder;
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, engine, QStringLiteral("function retained() return true end"), &runtime);

	QThread        firstThread;
	QObject        firstTarget;
	QThread *const testThread = QThread::currentThread();
	QSemaphore     firstDeliveryEntered;
	QSemaphore     releaseFirstDelivery;
	firstTarget.moveToThread(&firstThread);
	firstThread.start();
	const auto stopThread = qScopeGuard(
	    [&]
	    {
		    releaseFirstDelivery.release();
		    if (firstTarget.thread() == &firstThread && firstThread.isRunning())
		    {
			    static_cast<void>(qmudLuaBridgeInvokeOnObjectThread(
			        &firstTarget, [&firstTarget, testThread] { firstTarget.moveToThread(testThread); }));
		    }
		    firstThread.quit();
		    static_cast<void>(firstThread.wait());
	    });

	LuaBatchDispatchRequest request;
	request.kind              = LuaBatchDispatchKind::NoArgs;
	request.engines           = {engine};
	request.functionName      = QStringLiteral("retained");
	const auto appendMutation = [&](const QString &marker)
	{
		std::atomic_bool appended{false};
		executor.dispatchBatchAsync(
		    request, nullptr,
		    [engine, &runtime, &mutationOrder, &appended, marker](const LuaBatchDispatchResult &)
		    {
			    QVector<std::function<void()>> mutations;
			    mutations.push_back([&mutationOrder, marker] { mutationOrder.push_back(marker); });
			    engine->appendDeferredRuntimeMutationBatch(&runtime, std::move(mutations));
			    appended.store(true, std::memory_order_release);
		    });
		QTRY_VERIFY_WITH_TIMEOUT(appended.load(std::memory_order_acquire), 3000);
	};

	std::atomic_bool firstCompleted{false};
	std::atomic_bool secondCompleted{false};
	appendMutation(QStringLiteral("first"));
	executor.dispatchBatchAsync(
	    request, &firstTarget,
	    [&](const LuaBatchDispatchResult &dispatchResult)
	    {
		    LuaBatchDispatchResult                   result = dispatchResult;
		    QVector<LuaDeferredRuntimeMutationBatch> currentBatches;
		    currentBatches.swap(result.deferredRuntimeMutationBatches);
		    if (result.deferredRuntimeMutationDelivery)
		    {
			    static_cast<void>(result.deferredRuntimeMutationDelivery->consumeForDelivery(
			        [currentBatches = std::move(currentBatches), &firstDeliveryEntered,
			         &releaseFirstDelivery](QVector<LuaDeferredRuntimeMutationBatch> earlierBatches) mutable
			        {
				        firstDeliveryEntered.release();
				        releaseFirstDelivery.acquire();
				        earlierBatches += std::move(currentBatches);
				        QMudLuaDeferredRuntimeMutation::apply(std::move(earlierBatches));
			        }));
		    }
		    firstCompleted.store(true, std::memory_order_release);
	    });
	QVERIFY(firstDeliveryEntered.tryAcquire(1, 3000));
	appendMutation(QStringLiteral("second"));
	executor.dispatchBatchAsync(request, nullptr,
	                            [&](const LuaBatchDispatchResult &dispatchResult)
	                            {
		                            LuaBatchDispatchResult result = dispatchResult;
		                            QMudLuaDeferredRuntimeMutation::apply(result);
		                            secondCompleted.store(true, std::memory_order_release);
	                            });
	LuaBatchDispatchRequest barrier;
	barrier.kind = LuaBatchDispatchKind::HasFunction;
	static_cast<void>(executor.dispatchBatch(barrier));
	QVERIFY(!firstCompleted.load(std::memory_order_acquire));
	QTRY_VERIFY_WITH_TIMEOUT(secondCompleted.load(std::memory_order_acquire), 3000);
	QVERIFY(mutationOrder.isEmpty());

	releaseFirstDelivery.release();
	QTRY_VERIFY_WITH_TIMEOUT(firstCompleted.load(std::memory_order_acquire), 3000);
	QTRY_COMPARE_WITH_TIMEOUT(mutationOrder, QStringList({QStringLiteral("first"), QStringLiteral("second")}),
	                          3000);
}

void tst_LuaCallbackEngine::workerThrowingMutationCompletionRecoversAndAdvances()
{
	WorldRuntime      runtime;
	QStringList       mutationOrder;
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, engine, QStringLiteral("function retained() return true end"), &runtime);

	LuaBatchDispatchRequest request;
	request.kind                      = LuaBatchDispatchKind::NoArgs;
	request.engines                   = {engine};
	request.functionName              = QStringLiteral("retained");
	const auto appendMutationOnWorker = [&](const QString &marker)
	{
		std::atomic_bool appended{false};
		executor.dispatchBatchAsync(
		    request, nullptr,
		    [engine, &runtime, &mutationOrder, &appended, marker](const LuaBatchDispatchResult &)
		    {
			    QVector<std::function<void()>> mutations;
			    mutations.push_back([&mutationOrder, marker] { mutationOrder.push_back(marker); });
			    engine->appendDeferredRuntimeMutationBatch(&runtime, std::move(mutations));
			    appended.store(true, std::memory_order_release);
		    });
		QTRY_VERIFY_WITH_TIMEOUT(appended.load(std::memory_order_acquire), 3000);
	};

	appendMutationOnWorker(QStringLiteral("throwing"));
	QTest::ignoreMessage(
	    QtWarningMsg,
	    "[QMud][LuaExecutor] worker dispatch failed: exception while delivering a targetless asynchronous "
	    "completion: targetless mutation completion failure");
	executor.dispatchBatchAsync(request, nullptr, [](const LuaBatchDispatchResult &)
	                            { throw std::runtime_error("targetless mutation completion failure"); });
	LuaBatchDispatchRequest barrierRequest;
	barrierRequest.kind = LuaBatchDispatchKind::HasFunction;
	static_cast<void>(executor.dispatchBatch(barrierRequest));
	QTRY_COMPARE_WITH_TIMEOUT(mutationOrder, QStringList({QStringLiteral("throwing")}), 3000);

	appendMutationOnWorker(QStringLiteral("following"));
	QObject          followingTarget;
	std::atomic_bool followingCompleted{false};
	executor.dispatchBatchAsync(request, &followingTarget,
	                            [&](const LuaBatchDispatchResult &dispatchResult)
	                            {
		                            LuaBatchDispatchResult result = dispatchResult;
		                            QMudLuaDeferredRuntimeMutation::apply(result);
		                            followingCompleted.store(true, std::memory_order_release);
	                            });
	static_cast<void>(executor.dispatchBatch(barrierRequest));
	QTRY_VERIFY_WITH_TIMEOUT(followingCompleted.load(std::memory_order_acquire), 3000);
	QCOMPARE(mutationOrder, QStringList({QStringLiteral("throwing"), QStringLiteral("following")}));
}

void tst_LuaCallbackEngine::workerShutdownTeardownPreservesRetainedEngineOrder()
{
	WorldRuntime runtime;
	QStringList  teardownOrder;
	auto         executor = std::make_unique<LuaExecutorWorker>(
	    [](const QVector<LuaDeferredRuntimeMutationBatch> &batches)
	    {
		    for (const LuaDeferredRuntimeMutationBatch &batch : batches)
		    {
			    for (const std::function<void()> &mutation : batch.mutations)
			    {
				    if (mutation)
					    mutation();
			    }
		    }
	    });
	auto first  = QSharedPointer<LuaCallbackEngine>::create();
	auto second = QSharedPointer<LuaCallbackEngine>::create();
	auto third  = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(*executor, first, QStringLiteral("function retained() return true end"), &runtime,
	                       QStringLiteral("First"));
	initializeWorkerEngine(*executor, second, QStringLiteral("function retained() return true end"), &runtime,
	                       QStringLiteral("Second"));
	initializeWorkerEngine(*executor, third, QStringLiteral("function retained() return true end"), &runtime,
	                       QStringLiteral("Third"));
	teardownWorkerEngine(*executor, second);
	initializeWorkerEngine(*executor, second, QStringLiteral("function retained() return true end"), &runtime,
	                       QStringLiteral("Second"));

	QVector<QPair<QSharedPointer<LuaCallbackEngine>, QString>> retainedOrder = {
	    {first,  QStringLiteral("first") },
	    {third,  QStringLiteral("third") },
	    {second, QStringLiteral("second")},
	};
	for (const auto &[engine, marker] : retainedOrder)
	{
		LuaBatchDispatchRequest request;
		request.kind         = LuaBatchDispatchKind::NoArgs;
		request.engines      = {engine};
		request.functionName = QStringLiteral("retained");
		executor->dispatchBatchAsync(
		    request, nullptr,
		    [engine, &runtime, &teardownOrder, marker](const LuaBatchDispatchResult &)
		    {
			    QVector<std::function<void()>> mutations;
			    mutations.push_back([&teardownOrder, marker] { teardownOrder.push_back(marker); });
			    engine->appendDeferredRuntimeMutationBatch(&runtime, std::move(mutations));
		    });
	}
	LuaBatchDispatchRequest barrierRequest;
	barrierRequest.kind = LuaBatchDispatchKind::HasFunction;
	static_cast<void>(executor->dispatchBatch(barrierRequest));
	QVERIFY(teardownOrder.isEmpty());

	retainedOrder.clear();
	first.clear();
	second.clear();
	third.clear();
	executor.reset();
	QCOMPARE(teardownOrder,
	         QStringList({QStringLiteral("first"), QStringLiteral("third"), QStringLiteral("second")}));
}

void tst_LuaCallbackEngine::workerShutdownCompletesActiveRuntimeBridgeBeforeTeardown()
{
	QObject      bridgeTarget;
	WorldRuntime runtime;
	QStringList  shutdownOrder;
	QStringList  completionOrder;
	QVERIFY(qmudLuaBridgeEnsureObjectThreadReady(&bridgeTarget));

	auto executor = std::make_unique<LuaExecutorWorker>(
	    [](const QVector<LuaDeferredRuntimeMutationBatch> &batches)
	    {
		    for (const LuaDeferredRuntimeMutationBatch &batch : batches)
		    {
			    for (const std::function<void()> &mutation : batch.mutations)
			    {
				    if (mutation)
					    mutation();
			    }
		    }
	    });
	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(*executor, engine,
	                       QStringLiteral("function active_bridge() return test_runtime_bridge() end"),
	                       &runtime);

	ActiveWorkerBridgeContext context;
	context.target = &bridgeTarget;
	std::atomic_bool        registered{false};
	LuaBatchDispatchRequest registrationRequest;
	registrationRequest.kind         = LuaBatchDispatchKind::HasFunction;
	registrationRequest.engines      = {engine};
	registrationRequest.functionName = QStringLiteral("active_bridge");
	executor->dispatchBatchAsync(
	    registrationRequest, nullptr,
	    [engine, &context, &runtime, &shutdownOrder, &registered](const LuaBatchDispatchResult &)
	    {
		    lua_State *state = engine->luaState();
		    if (state)
		    {
			    lua_pushlightuserdata(state, &context);
			    lua_pushcclosure(state, activeWorkerBridge, 1);
			    lua_setglobal(state, "test_runtime_bridge");
			    QVector<std::function<void()>> mutations;
			    mutations.push_back([&shutdownOrder]
			                        { shutdownOrder.push_back(QStringLiteral("mutation")); });
			    engine->appendDeferredRuntimeMutationBatch(&runtime, std::move(mutations));
		    }
		    registered.store(state != nullptr, std::memory_order_release);
	    });
	QTRY_VERIFY_WITH_TIMEOUT(registered.load(std::memory_order_acquire), 3000);
	registrationRequest.engines.clear();

	std::atomic_bool        callbackCompleted{false};
	LuaBatchDispatchRequest callbackRequest;
	callbackRequest.kind         = LuaBatchDispatchKind::NoArgs;
	callbackRequest.engines      = {engine};
	callbackRequest.functionName = QStringLiteral("active_bridge");
	executor->dispatchBatchAsync(
	    callbackRequest, &bridgeTarget,
	    [&callbackCompleted, &completionOrder](const LuaBatchDispatchResult &dispatchResult)
	    {
		    LuaBatchDispatchResult result = dispatchResult;
		    QMudLuaDeferredRuntimeMutation::apply(result);
		    completionOrder.push_back(QStringLiteral("active"));
		    callbackCompleted.store(true, std::memory_order_release);
	    });
	QVERIFY(context.entered.tryAcquire(1, 3000));
	QVERIFY(!context.bridgeRan.load(std::memory_order_acquire));

	LuaBatchDispatchRequest pendingRequest;
	pendingRequest.kind         = LuaBatchDispatchKind::HasFunction;
	pendingRequest.engines      = {engine};
	pendingRequest.functionName = QStringLiteral("active_bridge");
	std::atomic_bool fallbackCompleted{false};
	executor->dispatchBatchAsync(pendingRequest, &bridgeTarget,
	                             [&fallbackCompleted, &completionOrder](const LuaBatchDispatchResult &)
	                             {
		                             completionOrder.push_back(QStringLiteral("fallback"));
		                             fallbackCompleted.store(true, std::memory_order_release);
	                             });
	auto             abandonedTarget = std::make_unique<QObject>();
	std::atomic_bool abandonedCompletion{false};
	executor->dispatchBatchAsync(pendingRequest, abandonedTarget.get(),
	                             [&abandonedCompletion](const LuaBatchDispatchResult &)
	                             { abandonedCompletion.store(true, std::memory_order_release); });
	abandonedTarget.reset();
	std::atomic_bool completionAfterAbandonedTarget{false};
	executor->dispatchBatchAsync(
	    pendingRequest, &bridgeTarget,
	    [&completionAfterAbandonedTarget, &completionOrder](const LuaBatchDispatchResult &)
	    {
		    completionOrder.push_back(QStringLiteral("after-abandoned"));
		    completionAfterAbandonedTarget.store(true, std::memory_order_release);
	    });

	const QWeakPointer<LuaCallbackEngine> weakEngine = engine;
	callbackRequest.engines.clear();
	pendingRequest.engines.clear();
	engine.clear();
	executor.reset();

	QVERIFY(context.bridgeRan.load(std::memory_order_acquire));
	QCOMPARE(shutdownOrder, QStringList({QStringLiteral("mutation")}));
	QVERIFY(completionOrder.isEmpty());
	QTRY_VERIFY_WITH_TIMEOUT(callbackCompleted.load(std::memory_order_acquire), 3000);
	QTRY_VERIFY_WITH_TIMEOUT(fallbackCompleted.load(std::memory_order_acquire), 3000);
	QTRY_VERIFY_WITH_TIMEOUT(completionAfterAbandonedTarget.load(std::memory_order_acquire), 3000);
	QCOMPARE(completionOrder, QStringList({QStringLiteral("active"), QStringLiteral("fallback"),
	                                       QStringLiteral("after-abandoned")}));
	QVERIFY(!abandonedCompletion.load(std::memory_order_acquire));
	QVERIFY(weakEngine.isNull());
}

void tst_LuaCallbackEngine::lineStyleSnapshotRoundTripPreservesColourState()
{
	WorldRuntime::StyleSpan source;
	source.length = 4;
	source.fore   = QColor();
	source.back   = QColor(10, 20, 30, 40);

	const LuaCallbackLineStyleSnapshot snapshot = QMudLuaCallbackLineSnapshot::fromStyleSpan(source);
	QVERIFY(!snapshot.foreValid);
	QVERIFY(snapshot.backValid);
	QCOMPARE(QColor::fromRgba(snapshot.backRgba), source.back);
	const WorldRuntime::StyleSpan restored = QMudLuaCallbackLineSnapshot::toStyleSpan(snapshot);
	QVERIFY(!restored.fore.isValid());
	QCOMPARE(restored.back, source.back);
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
	WorldRuntime      runtime;
	int               value = 0;
	engine.appendDeferredRuntimeMutationBatch(&runtime, {std::function<void()>([&value] { value += 1; }),
	                                                     std::function<void()>([&value] { value *= 10; })});

	QVector<LuaDeferredRuntimeMutationBatch> batches = engine.takeDeferredRuntimeMutationBatches();
	QCOMPARE(batches.size(), 1);
	QVERIFY(batches.first().runtime == &runtime);
	QCOMPARE(batches.first().mutations.size(), 2);
	for (auto &mutation : batches.first().mutations)
		mutation();
	QCOMPARE(value, 10);
	QVERIFY(engine.takeDeferredRuntimeMutationBatches().isEmpty());

	LuaDeferredRuntimeMutationBatch nested;
	nested.runtime = &runtime;
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
	QVERIFY(result.hasFunctionValid);
	QVERIFY(result.hasFunction);

	request.functionName  = QStringLiteral("missing_stop_false");
	request.defaultResult = false;
	result                = executor.dispatchBatch(request);
	QVERIFY(result.boolResultValid);
	QVERIFY(result.boolResult);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(!result.hasFunction);

	request.kind          = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.functionName  = QStringLiteral("missing_stop_true");
	request.defaultResult = true;
	result                = executor.dispatchBatch(request);
	QVERIFY(result.boolResultValid);
	QVERIFY(!result.boolResult);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(!result.hasFunction);

	request.kind          = LuaBatchDispatchKind::StringHandled;
	request.functionName  = QStringLiteral("string_handled");
	request.stringArg     = QStringLiteral("handled");
	request.defaultResult = true;
	result                = executor.dispatchBatch(request);
	QVERIFY(result.boolResultValid);
	QVERIFY(result.boolResult);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(result.hasFunction);

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

void tst_LuaCallbackEngine::setOptionUpdatesOnlyTabCompletionSymbolBehaviors()
{
	WorldRuntime runtime;
	runtime.applyDefaultWorldOptions();
	runtime.setWorldAttribute(QStringLiteral("tab_completion_space"), QStringLiteral("0"));
	runtime.addLine(QStringLiteral("Nodens:"), WorldRuntime::LineOutput);

	WorldView view;
	view.resize(860, 520);
	view.setRuntime(&runtime);
	view.show();
	QCoreApplication::processEvents();

	QPlainTextEdit *const input = view.inputEditor();
	QVERIFY(input);
	input->setFocus();
	// Leave an unrelated runtime option deliberately out of sync with the view. SetOption must not
	// reapply it while changing either symbol-exclusion option.
	runtime.setWorldAttribute(QStringLiteral("lower_case_tab_completion"), QStringLiteral("1"));
	view.setInputText(QStringLiteral("@node"), true);
	QTest::keyClick(input, Qt::Key_Tab);
	QCOMPARE(view.inputText(), QStringLiteral("@Nodens"));

	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
function disable_symbol_suffix_exclusion()
  SetOption("tab_completion_excludes_symbol_suffix", 0)
end
function disable_symbol_prefix_exclusion()
  SetOption("tab_completion_excludes_symbol_prefix", 0)
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines                       = {engine};
	request.kind                          = LuaBatchDispatchKind::NoArgs;
	request.functionName                  = QStringLiteral("disable_symbol_suffix_exclusion");
	request.miniWindowSnapshotArg         = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult callbackResult = executor.dispatchBatch(request);
	executeDeferredMutations(callbackResult);
	QCOMPARE(runtime.worldAttributes().value(QStringLiteral("tab_completion_excludes_symbol_suffix")),
	         QStringLiteral("0"));

	view.setInputText(QStringLiteral("@node"), true);
	QTest::keyClick(input, Qt::Key_Tab);
	QCOMPARE(view.inputText(), QStringLiteral("@Nodens:"));

	request.functionName          = QStringLiteral("disable_symbol_prefix_exclusion");
	request.miniWindowSnapshotArg = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	callbackResult                = executor.dispatchBatch(request);
	executeDeferredMutations(callbackResult);
	QCOMPARE(runtime.worldAttributes().value(QStringLiteral("tab_completion_excludes_symbol_prefix")),
	         QStringLiteral("0"));

	view.setInputText(QStringLiteral("@node"), true);
	QTest::keyClick(input, Qt::Key_Tab);
	QCOMPARE(view.inputText(), QStringLiteral("@node"));

	view.setInputText(QStringLiteral("node"), true);
	QTest::keyClick(input, Qt::Key_Tab);
	QCOMPARE(view.inputText(), QStringLiteral("Nodens:"));
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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

void tst_LuaCallbackEngine::workerSingleRecipientDispatchesDrainDeferredMutations()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	const auto teardown = qScopeGuard([&executor, &engine] { teardownWorkerEngine(executor, engine); });
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function procedure_output(value)
  Note("procedure:" .. value)
end
function mxp_error_output(error_type, line, column, message)
  Note("mxp error:" .. message)
  return false
end
function mxp_start_tag_output(name, arguments, attributes)
  Note("mxp start:" .. name)
  return true
end
)lua"),
	                       &runtime);

	auto captureSnapshot = [&runtime]
	{
		quint64                             generation = 0;
		QHash<int, WorldRuntime::LineEntry> entries;
		QStringList                         recentLines;
		const int count = runtime.luaContextLinePageByBufferIndex(0, 0, generation, entries, recentLines);
		auto      lineSnapshot             = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
		lineSnapshot->lineBufferGeneration = generation;
		lineSnapshot->lineBufferCount      = count;
		auto snapshot                      = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
		snapshot->hasLineBufferSnapshot    = true;
		snapshot->lineBufferCount          = count;
		snapshot->lineBufferSnapshot       = lineSnapshot;
		if (count > 0)
		{
			snapshot->hasCallbackOutputAnchor            = true;
			snapshot->callbackOutputAnchorBufferIndex    = count;
			snapshot->callbackOutputAnchorAbsoluteNumber = runtime.lines().constLast().lineNumber;
		}
		return snapshot;
	};
	auto dispatchAndApply =
	    [&](LuaBatchDispatchRequest request, const QString &expectedOutput, LuaBatchDispatchResult &result)
	{
		request.engines               = {engine};
		request.miniWindowSnapshotArg = captureSnapshot();
		dispatchWorkerAndWait(executor, request, result);
		QVERIFY(!result.deferredRuntimeMutationBatches.isEmpty());
		executeDeferredMutations(result);
		QCOMPARE(runtime.lines().constLast().text, expectedOutput);
	};

	LuaBatchDispatchRequest procedure;
	procedure.kind         = LuaBatchDispatchKind::ProcedureWithString;
	procedure.functionName = QStringLiteral("procedure_output");
	procedure.stringArg    = QStringLiteral("payload");
	LuaBatchDispatchResult procedureResult;
	dispatchAndApply(procedure, QStringLiteral("procedure:payload"), procedureResult);
	QVERIFY(procedureResult.boolResultValid);
	QVERIFY(procedureResult.boolResult);
	QVERIFY(procedureResult.hasFunctionValid);
	QVERIFY(procedureResult.hasFunction);

	LuaBatchDispatchRequest mxpError;
	mxpError.kind         = LuaBatchDispatchKind::MxpError;
	mxpError.functionName = QStringLiteral("mxp_error_output");
	mxpError.intArg1      = 2;
	mxpError.numberArg1   = 42;
	mxpError.intArg2      = 7;
	mxpError.stringArg    = QStringLiteral("bad tag");
	LuaBatchDispatchResult mxpErrorResult;
	dispatchAndApply(mxpError, QStringLiteral("mxp error:bad tag"), mxpErrorResult);
	QVERIFY(mxpErrorResult.boolResultValid);
	QVERIFY(!mxpErrorResult.boolResult);

	LuaBatchDispatchRequest mxpStartTag;
	mxpStartTag.kind         = LuaBatchDispatchKind::MxpStartTag;
	mxpStartTag.functionName = QStringLiteral("mxp_start_tag_output");
	mxpStartTag.stringArg    = QStringLiteral("send");
	mxpStartTag.stringArg2   = QStringLiteral("href='look'");
	mxpStartTag.mapArg.insert(QStringLiteral("href"), QStringLiteral("look"));
	LuaBatchDispatchResult mxpStartTagResult;
	dispatchAndApply(mxpStartTag, QStringLiteral("mxp start:send"), mxpStartTagResult);
	QVERIFY(mxpStartTagResult.boolResultValid);
	QVERIFY(mxpStartTagResult.boolResult);

	LuaBatchDispatchRequest executeScript;
	executeScript.kind       = LuaBatchDispatchKind::ExecuteScript;
	executeScript.stringArg  = QStringLiteral("Note('execute script')");
	executeScript.stringArg2 = QStringLiteral("worker deferred mutation regression");
	LuaBatchDispatchResult executeScriptResult;
	dispatchAndApply(executeScript, QStringLiteral("execute script"), executeScriptResult);
	QVERIFY(executeScriptResult.boolResultValid);
	QVERIFY(executeScriptResult.boolResult);
}

void tst_LuaCallbackEngine::workerSqliteResourcesOutliveCreatingCallbackCoroutine()
{
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	const auto teardown = qScopeGuard([&executor, &engine] { teardownWorkerEngine(executor, engine); });
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
retained_sqlite = nil
closing_sqlite = nil
abandoned_sqlite = nil
abandoned_weak = nil

local function new_database(value)
  local db = assert(sqlite3.open_memory())
  assert(db:execute(string.format([[
    CREATE TABLE values_table(value INTEGER);
    INSERT INTO values_table(value) VALUES (%d);
  ]], value)))
  return db
end

local function retain_resources(global_name, value)
  local db = new_database(value)
  local db_next, db_iter = db:nrows("SELECT value FROM values_table")
  local stmt = assert(db:prepare("SELECT value FROM values_table"))
  local stmt_next, stmt_iter = stmt:nrows()
  local resources = {
    db = db,
    db_next = db_next,
    db_iter = db_iter,
    stmt = stmt,
    stmt_next = stmt_next,
    stmt_iter = stmt_iter,
  }
  _G[global_name] = resources
  return resources
end

function retain_sqlite_resources()
  retain_resources("retained_sqlite", 7)
end

function consume_retained_sqlite_resources(_)
  collectgarbage("collect")
  collectgarbage("collect")

  local db_row = assert(retained_sqlite.db_next())
  assert(db_row.value == 7)
  assert(retained_sqlite.db_next() == nil)

  local stmt_row = assert(retained_sqlite.stmt_next())
  assert(stmt_row.value == 7)
  assert(retained_sqlite.stmt_next() == nil)

  assert(retained_sqlite.stmt:finalize() == 0)
  assert(retained_sqlite.db:close() == 0)
  retained_sqlite = nil
  collectgarbage("collect")
  return "consumed"
end

function retain_sqlite_resources_for_close()
  retain_resources("closing_sqlite", 13)
end

function close_sqlite_with_active_resources(_)
  collectgarbage("collect")
  collectgarbage("collect")

  assert(closing_sqlite.db:close() == 0)
  assert(closing_sqlite.db_next() == nil)
  assert(closing_sqlite.stmt_next() == nil)
  closing_sqlite = nil
  collectgarbage("collect")
  return "closed"
end

function retain_abandoned_sqlite_resources()
  local resources = retain_resources("abandoned_sqlite", 11)
  abandoned_weak = setmetatable({}, { __mode = "v" })
  for name, value in pairs(resources) do
    abandoned_weak[name] = value
  end
end

function collect_abandoned_sqlite_resources(_)
  collectgarbage("collect")
  collectgarbage("collect")

  abandoned_sqlite = nil
  for _ = 1, 6 do
    collectgarbage("collect")
  end
  local resources_collected = next(abandoned_weak) == nil
  abandoned_weak = nil

  local db = new_database(19)
  local value = nil
  for row in db:nrows("SELECT value FROM values_table") do
    value = row.value
  end
  assert(db:close() == 0)
  return tostring(value) .. ":" .. tostring(resources_collected)
end
)lua"));

	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("retain_sqlite_resources");
	dispatchWorkerAndWait(executor, request);

	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("consume_retained_sqlite_resources");
	request.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("consumed"));

	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("retain_sqlite_resources_for_close");
	request.stringArg.clear();
	dispatchWorkerAndWait(executor, request);

	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("close_sqlite_with_active_resources");
	request.stringArg    = QStringLiteral("ignored");
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("closed"));

	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("retain_abandoned_sqlite_resources");
	request.stringArg.clear();
	dispatchWorkerAndWait(executor, request);

	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("collect_abandoned_sqlite_resources");
	request.stringArg    = QStringLiteral("ignored");
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("19:true"));
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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

void tst_LuaCallbackEngine::nativeShimDiscoveryRespectsShadowPluginVisibility()
{
	WorldRuntime  runtime;
	QTemporaryDir soundRoot;
	QVERIFY(soundRoot.isValid());
	QVERIFY(QDir(soundRoot.path()).mkpath(QStringLiteral("sounds")));
	QFile soundFile(QDir(soundRoot.path()).filePath(QStringLiteral("sounds/coin.wav")));
	QVERIFY(soundFile.open(QIODevice::WriteOnly));
	const QByteArray soundData = QMudTestSoundData::silentPcmWave();
	QCOMPARE(soundFile.write(soundData), static_cast<qint64>(soundData.size()));
	soundFile.close();
	runtime.setStartupDirectory(soundRoot.path());
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	    tostring(IsPluginInstalled(id)),
	    tostring(IsPluginInstalled("MushReader")),
	    GetPluginInfo(id, 1) or "",
	    tostring(GetPluginInfo(id, 17) or false),
	    string.format("%.0f", PluginSupports(id, "say")),
	    string.format("%.0f", PluginSupports("MushReader", "say")),
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
	seedPluginMetadataDispatchSnapshotForTest(snapshot, runtime);

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
	QCOMPARE(result.stringResult,
	         QStringLiteral("false|false||false|%1|%1|LuaAudio|true|100|1|false").arg(eNoSuchPlugin));
	QVERIFY(!runtime.pluginIdList().contains(shimId, Qt::CaseInsensitive));
	QVERIFY(
	    runtime.pluginIdList().contains(QMudNativePluginRegistry::luaAudioPluginId(), Qt::CaseInsensitive));
	QVERIFY(!runtime.isPluginInstalled(shimId));
	QVERIFY(!runtime.pluginInfo(shimId, 1).isValid());
	QCOMPARE(runtime.pluginSupports(shimId, QStringLiteral("say")), eNoSuchPlugin);
	QVERIFY(runtime.isPluginInstalled(QMudNativePluginRegistry::luaAudioPluginId()));
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::nativeMushReaderEnableByNameUpdatesResolvedCallbackMetadata()
{
	const QString        mushReaderId = QMudNativePluginRegistry::mushReaderPluginId();

	WorldRuntime::Plugin mushReaderPlugin;
	mushReaderPlugin.enabled    = true;
	mushReaderPlugin.nativeShim = true;
	mushReaderPlugin.attributes.insert(QStringLiteral("id"), mushReaderId);
	mushReaderPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("MushReader"));

	WorldRuntime runtime;
	runtime.pluginsMutable().push_back(mushReaderPlugin);

	QVector<QMudNativePluginRegistry::TestSpeechEvent> speechEvents;
	QMudNativePluginRegistry::setTestSpeechSink(
	    [&speechEvents](const QMudNativePluginRegistry::TestSpeechEvent &event)
	    { speechEvents.push_back(event); });
	const auto restoreSpeechSink = qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });

	auto       engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local id = "925cdd0331023d9f0b8f05a7"
	  local code = EnablePlugin("MushReader", false)
	  local call_code, message = CallPlugin(id, "say", "blocked")
	  mushreader_enable_by_name_status = table.concat({
	    string.format("%.0f", code),
	    tostring(IsPluginInstalled(id)),
	    tostring(GetPluginInfo(id, 17) or false),
	    string.format("%.0f", PluginSupports(id, "say")),
	    string.format("%.0f", call_code),
	    tostring((message or ""):find("disabled", 1, true) ~= nil)
	  }, "|")
	end
	function mushreader_enable_by_name_result(value)
	  return mushreader_enable_by_name_status
	end
	)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("OnPluginEnable");
	auto snapshot        = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	seedPluginMetadataDispatchSnapshotForTest(snapshot, runtime);
	request.miniWindowSnapshotArg = snapshot;
	dispatchWorkerAndWait(executor, request);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("mushreader_enable_by_name_result");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult,
	         QStringLiteral("%1|true|false|%2|%3|true").arg(eOK).arg(eOK).arg(ePluginDisabled));
	QVERIFY(speechEvents.isEmpty());
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::nativeMushReaderCallPluginUsesCallbackSpeechSnapshot()
{
	const QString        mushReaderId = QMudNativePluginRegistry::mushReaderPluginId();

	WorldRuntime::Plugin mushReaderPlugin;
	mushReaderPlugin.enabled    = true;
	mushReaderPlugin.nativeShim = true;
	mushReaderPlugin.attributes.insert(QStringLiteral("id"), mushReaderId);
	mushReaderPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("MushReader"));

	WorldRuntime runtime;
	runtime.pluginsMutable().push_back(mushReaderPlugin);
	QVector<QMudNativePluginRegistry::TestSpeechEvent> speechEvents;
	QMudNativePluginRegistry::setTestSpeechSink(
	    [&speechEvents](const QMudNativePluginRegistry::TestSpeechEvent &event)
	    { speechEvents.push_back(event); });
	const auto restoreSpeechSink = qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });
	QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);
	QVERIFY(QMudNativePluginRegistry::handleMushReaderCommand(&runtime, QStringLiteral("tts")));
	QVERIFY(!QMudNativePluginRegistry::isMushReaderSpeechEnabled(&runtime));
	speechEvents.clear();

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local id = "925cdd0331023d9f0b8f05a7"
	  local say_code = CallPlugin(id, "say", "muted")
	  local interrupt_code = CallPlugin("MushReader", "interrupt", "muted interrupt")
	  local update_code, update_url = CallPlugin(id, "plugin_update_url")
	  mushreader_muted_call_status = table.concat({
	    string.format("%.0f", say_code),
	    string.format("%.0f", interrupt_code),
	    string.format("%.0f", update_code),
	    tostring(update_url)
	  }, "|")
	end
	function mushreader_muted_call_result(value)
	  return mushreader_muted_call_status
	end
	)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("OnPluginEnable");
	auto snapshot        = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	seedPluginMetadataDispatchSnapshotForTest(snapshot, runtime);
	request.miniWindowSnapshotArg = snapshot;
	dispatchWorkerAndWait(executor, request);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("mushreader_muted_call_result");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("%1|%1|%1|qmud:native/MushReader").arg(eOK));
	QVERIFY(speechEvents.isEmpty());
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::nativeMushReaderCallPluginDefersSpeechToRuntimeThread()
{
	const QString        mushReaderId = QMudNativePluginRegistry::mushReaderPluginId();

	WorldRuntime::Plugin mushReaderPlugin;
	mushReaderPlugin.enabled    = true;
	mushReaderPlugin.nativeShim = true;
	mushReaderPlugin.attributes.insert(QStringLiteral("id"), mushReaderId);
	mushReaderPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("MushReader"));

	WorldRuntime runtime;
	runtime.pluginsMutable().push_back(mushReaderPlugin);
	QVector<QMudNativePluginRegistry::TestSpeechEvent> speechEvents;
	QMudNativePluginRegistry::setTestSpeechSink(
	    [&speechEvents](const QMudNativePluginRegistry::TestSpeechEvent &event)
	    { speechEvents.push_back(event); });
	const auto restoreSpeechSink = qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });
	QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);
	speechEvents.clear();

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local id = "925cdd0331023d9f0b8f05a7"
	  local say_code = CallPlugin(id, "say", "deferred")
	  local interrupt_code = CallPlugin("MushReader", "interrupt", "deferred interrupt")
	  mushreader_deferred_call_status = table.concat({
	    string.format("%.0f", say_code),
	    string.format("%.0f", interrupt_code)
	  }, "|")
	end
	function mushreader_deferred_call_result(value)
	  return mushreader_deferred_call_status
	end
	)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("OnPluginEnable");
	auto snapshot        = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	seedPluginMetadataDispatchSnapshotForTest(snapshot, runtime);
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult dispatchResult;
	dispatchWorkerAndWait(executor, request, dispatchResult);
	QVERIFY(speechEvents.isEmpty());
	QVERIFY(!dispatchResult.deferredRuntimeMutationBatches.isEmpty());

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("mushreader_deferred_call_result");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("%1|%1").arg(eOK));
	QVERIFY(speechEvents.isEmpty());

	executeDeferredMutations(dispatchResult);
	QCOMPARE(speechEvents.size(), 2);
	QCOMPARE(speechEvents.at(0).text, QStringLiteral("       deferred"));
	QVERIFY(!speechEvents.at(0).interrupt);
	QCOMPARE(speechEvents.at(1).text, QStringLiteral("deferred interrupt"));
	QVERIFY(speechEvents.at(1).interrupt);
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::nativeMushReaderDeferredCallPluginUsesRuntimeSpeechState()
{
	const QString        mushReaderId = QMudNativePluginRegistry::mushReaderPluginId();

	WorldRuntime::Plugin mushReaderPlugin;
	mushReaderPlugin.enabled    = true;
	mushReaderPlugin.nativeShim = true;
	mushReaderPlugin.attributes.insert(QStringLiteral("id"), mushReaderId);
	mushReaderPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("MushReader"));

	WorldRuntime          runtime;
	WorldCommandProcessor processor;
	processor.setRuntime(&runtime);
	runtime.setCommandProcessor(&processor);
	const auto clearCommandProcessor = qScopeGuard(
	    [&runtime, &processor]
	    {
		    runtime.setCommandProcessor(nullptr);
		    processor.setRuntime(nullptr);
	    });
	runtime.pluginsMutable().push_back(mushReaderPlugin);

	QVector<QMudNativePluginRegistry::TestSpeechEvent> speechEvents;
	QMudNativePluginRegistry::setTestSpeechSink(
	    [&speechEvents](const QMudNativePluginRegistry::TestSpeechEvent &event)
	    { speechEvents.push_back(event); });
	const auto restoreSpeechSink = qScopeGuard([] { QMudNativePluginRegistry::setTestSpeechSink({}); });
	QMudNativePluginRegistry::setMushReaderPluginEnabled(&runtime, true);
	speechEvents.clear();

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local id = "925cdd0331023d9f0b8f05a7"
	  local execute_code = Execute("tts")
	  local say_code = CallPlugin(id, "say", "must not speak")
	  mushreader_runtime_state_status = table.concat({
	    string.format("%.0f", execute_code),
	    string.format("%.0f", say_code)
	  }, "|")
	end
	function mushreader_runtime_state_result(value)
	  return mushreader_runtime_state_status
	end
	)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("OnPluginEnable");
	auto snapshot        = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	seedPluginMetadataDispatchSnapshotForTest(snapshot, runtime);
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult dispatchResult;
	dispatchWorkerAndWait(executor, request, dispatchResult);
	QVERIFY(speechEvents.isEmpty());
	QVERIFY(!dispatchResult.deferredRuntimeMutationBatches.isEmpty());

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("mushreader_runtime_state_result");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("%1|%1").arg(eOK));

	executeDeferredMutations(dispatchResult);
	QVERIFY(!QMudNativePluginRegistry::isMushReaderSpeechEnabled(&runtime));
	QCOMPARE(speechEvents.size(), 2);
	QVERIFY(speechEvents.at(0).stop);
	QCOMPARE(speechEvents.at(1).text, QStringLiteral("speech off"));
	QVERIFY(speechEvents.at(1).interrupt);
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::nativeLuaAudioSharedRuntimeStateCoversDirectAndCallPlugin()
{
	QSet<int>                      activeSoundBuffers;
	WorldRuntime                   runtime;
	WorldRuntime::TestSoundBackend soundBackend;
	soundBackend.play = [&activeSoundBuffers](const int buffer, const QString &fileName, const bool,
	                                          const double, const double)
	{
		if (buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
			return eBadParameter;
		if (fileName.isEmpty())
			return activeSoundBuffers.contains(buffer) ? eOK : eCannotPlaySound;
		activeSoundBuffers.insert(buffer);
		return eOK;
	};
	soundBackend.stop = [&activeSoundBuffers](const int buffer)
	{
		if (buffer == 0)
		{
			activeSoundBuffers.clear();
			return eOK;
		}
		if (buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
			return eBadParameter;
		activeSoundBuffers.remove(buffer);
		return eOK;
	};
	soundBackend.status = [&activeSoundBuffers](const int buffer)
	{
		if (buffer < 1 || buffer > WorldRuntime::kMaxSoundBuffers)
			return -1;
		return activeSoundBuffers.contains(buffer) ? 1 : -2;
	};
	soundBackend.reusable = [&activeSoundBuffers](const int buffer)
	{
		return buffer >= 1 && buffer <= WorldRuntime::kMaxSoundBuffers &&
		       !activeSoundBuffers.contains(buffer);
	};
	runtime.setSoundBackendForTest(std::move(soundBackend));

	QTemporaryDir soundRoot;
	QVERIFY(soundRoot.isValid());
	QVERIFY(QDir(soundRoot.path()).mkpath(QStringLiteral("sounds")));
	QFile soundFile(QDir(soundRoot.path()).filePath(QStringLiteral("sounds/coin.wav")));
	QVERIFY(soundFile.open(QIODevice::WriteOnly));
	const QByteArray soundData = QMudTestSoundData::silentPcmWave();
	QCOMPARE(soundFile.write(soundData), static_cast<qint64>(soundData.size()));
	soundFile.close();
	runtime.setStartupDirectory(soundRoot.path());

	QMudNativePluginRegistry::NativeCallContext audioContext;
	audioContext.installed                                         = true;
	audioContext.pluginEnabled                                     = true;
	audioContext.pluginName                                        = QStringLiteral("LuaAudio");
	const QMudNativePluginRegistry::NativeCallResult nativeDelayed = QMudNativePluginRegistry::callRoutine(
	    &runtime, QMudNativePluginRegistry::luaAudioPluginId(), QStringLiteral("playDelay"),
	    {QStringLiteral("coin.wav"), 10.0, 0.0, 100.0}, audioContext);
	QCOMPARE(nativeDelayed.errorCode, eOK);
	QCOMPARE(nativeDelayed.returnValues.size(), 1);
	QCOMPARE(nativeDelayed.returnValues.constFirst().toInt(), 1);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	activeSoundBuffers.insert(1);
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
	activeSoundBuffers.remove(1);

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
	activeSoundBuffers.insert(1);
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
	activeSoundBuffers.remove(1);

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
	activeSoundBuffers.insert(1);
	activeSoundBuffers.insert(2);
	activeSoundBuffers.insert(3);
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
	activeSoundBuffers.insert(1);
	activeSoundBuffers.insert(2);
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
	QVERIFY(!activeSoundBuffers.contains(2));
	teardownWorkerEngine(executor, timedEngine);
	QVERIFY(QMudNativePluginRegistry::luaAudioRuntimeOwnedBuffers(&runtime).isEmpty());
	QVERIFY(activeSoundBuffers.isEmpty());
}

void tst_LuaCallbackEngine::disabledNativeLuaAudioShadowBlocksCallbackCallPluginFastPath()
{
	const QString        audioId = QMudNativePluginRegistry::luaAudioPluginId();

	WorldRuntime::Plugin audioPlugin;
	audioPlugin.enabled    = false;
	audioPlugin.nativeShim = true;
	audioPlugin.attributes.insert(QStringLiteral("id"), audioId);
	audioPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("LuaAudio"));

	WorldRuntime runtime;
	runtime.pluginsMutable().push_back(audioPlugin);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
	function OnPluginEnable()
	  local audio_id = "aedf0cb0be5bf045860d54b7"
	  local enabled = tostring(GetPluginInfo(audio_id, 17) or false)
	  local code, message = CallPlugin(audio_id, "plugin_update_url")
	  lua_audio_disabled_call = table.concat({
	    enabled,
	    string.format("%.0f", code),
	    tostring((message or ""):find("disabled", 1, true) ~= nil)
	  }, "|")
	end
	function lua_audio_disabled_status(value)
	  return lua_audio_disabled_call
	end
	)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines      = {engine};
	request.kind         = LuaBatchDispatchKind::NoArgs;
	request.functionName = QStringLiteral("OnPluginEnable");
	auto snapshot        = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	seedPluginMetadataDispatchSnapshotForTest(snapshot, runtime);
	request.miniWindowSnapshotArg = snapshot;
	dispatchWorkerAndWait(executor, request);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("lua_audio_disabled_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("false|%1|true").arg(ePluginDisabled));
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::blacklistedPluginsAreHiddenFromPluginApis()
{
	WorldRuntime      runtime;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{0});
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::triggerSnapshotPreservesPresentationCountAndIndexes()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 300; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
	const qint64 triggerLineNumber = runtime.lines().constLast().lineNumber;
	const qint64 firstLineNumber   = runtime.lines().constFirst().lineNumber;
	QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(
	    firstLineNumber, 1, false, QStringLiteral("inserted after first"), WorldRuntime::LineNote, {}, true));
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 301);

	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
trigger_snapshot_result = ""
function trigger_snapshot_cb(name, line, wildcards)
  trigger_snapshot_result = string.format("%.0f|%s", GetLinesInBufferCount(), tostring(GetLineInfo(2, 1)))
end
function trigger_snapshot_status(value)
  return trigger_snapshot_result
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines        = {engine};
	request.kind           = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName   = QStringLiteral("trigger_snapshot_cb");
	request.stringListArg  = {QStringLiteral("trigger"), QStringLiteral("line 300")};
	request.stringListArg2 = {QStringLiteral("line 300")};
	const auto styleRuns   = QSharedPointer<QVector<LuaStyleRun>>::create();
	styleRuns->push_back({QStringLiteral("line 300"), 0xFFFFFF, 0x000000, 0});
	request.styleRunsArg                     = styleRuns;
	request.triggerMatchedLineBufferIndex    = 301;
	request.triggerMatchedLineAbsoluteNumber = triggerLineNumber;
	request.miniWindowSnapshotArg            = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	LuaBatchDispatchResult triggerResult     = executor.dispatchBatch(request);
	QVERIFY(triggerResult.suspended);
	QVERIFY(triggerResult.hasPendingModalStringRequest);
	QVERIFY(triggerResult.pendingModalStringRequest.beforeRuntimeResumeCallback);
	triggerResult.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
	LuaBatchDispatchRequest resume;
	resume.engines       = {engine};
	resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	resume.modalResumeId = triggerResult.modalResumeId;
	triggerResult        = executor.dispatchBatch(resume);
	QVERIFY(!triggerResult.suspended);

	request.kind                        = LuaBatchDispatchKind::StringInOut;
	request.functionName                = QStringLiteral("trigger_snapshot_status");
	request.stringArg                   = QStringLiteral("ignored");
	request.miniWindowSnapshotArg       = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	const LuaBatchDispatchResult result = executor.dispatchBatch(request);
	QCOMPARE(result.stringResult, QStringLiteral("301|inserted after first"));
}

void tst_LuaCallbackEngine::triggerSnapshotPreservesMatchedMetadataAndRecentPresentation()
{
	WorldRuntime            runtime;
	WorldRuntime::LineEntry first;
	first.text       = QStringLiteral("first");
	first.lineNumber = 41;
	WorldRuntime::LineEntry matched;
	matched.text       = QStringLiteral("matched from buffer");
	matched.flags      = WorldRuntime::LineNote | WorldRuntime::LineBookmark;
	matched.hardReturn = false;
	matched.time       = QDateTime::fromSecsSinceEpoch(1700000000);
	matched.lineNumber = 42;
	matched.ticks      = 1.25;
	matched.elapsed    = 2.5;
	WorldRuntime::LineEntry shiftedIntoOldIndex;
	shiftedIntoOldIndex.text       = QStringLiteral("shifted into old index");
	shiftedIntoOldIndex.flags      = WorldRuntime::LineInput;
	shiftedIntoOldIndex.lineNumber = 99;
	WorldRuntime::LineEntry newest;
	newest.text       = QStringLiteral("newest presentation");
	newest.lineNumber = 43;
	runtime.replaceOutputLines({first, shiftedIntoOldIndex, matched, newest});

	auto lineSnapshot                  = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration = 1;
	lineSnapshot->lineBufferCount      = 4;
	LuaCallbackLineEntrySnapshot shiftedSnapshot;
	shiftedSnapshot.text       = shiftedIntoOldIndex.text;
	shiftedSnapshot.flags      = shiftedIntoOldIndex.flags;
	shiftedSnapshot.lineNumber = shiftedIntoOldIndex.lineNumber;
	lineSnapshot->lineEntriesByBufferIndex.insert(2, shiftedSnapshot);
	LuaCallbackLineEntrySnapshot matchedSnapshot;
	matchedSnapshot.text                         = matched.text;
	matchedSnapshot.flags                        = matched.flags;
	matchedSnapshot.hardReturn                   = matched.hardReturn;
	matchedSnapshot.time                         = matched.time;
	matchedSnapshot.lineNumber                   = matched.lineNumber;
	matchedSnapshot.ticks                        = matched.ticks;
	matchedSnapshot.elapsed                      = matched.elapsed;
	auto snapshot                                = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot              = true;
	snapshot->lineBufferCount                    = 4;
	snapshot->lineBufferSnapshot                 = lineSnapshot;
	snapshot->triggerMatchedLineSnapshotResolved = true;
	snapshot->hasTriggerMatchedLineSnapshot      = true;
	snapshot->triggerMatchedLineBufferIndex      = 3;
	snapshot->triggerMatchedLineAbsoluteNumber   = matched.lineNumber;
	snapshot->triggerMatchedLineSnapshot         = matchedSnapshot;
	snapshot->hasRecentLinesSnapshot             = true;
	snapshot->recentLinesSnapshot                = {newest.text};

	auto engine = QSharedPointer<LuaCallbackEngine>::create();
	engine->setWorldRuntime(&runtime);
	setEngineScript(*engine, QStringLiteral(R"lua(
trigger_metadata_result = ""
function trigger_metadata_cb(name, line, wildcards)
  trigger_metadata_result = string.format("%s|%s|%.0f|%.0f|%.2f|%.2f|%s",
    tostring(GetLineInfo(3, 4)), tostring(GetLineInfo(3, 7)), GetLineInfo(3, 9),
    GetLineInfo(3, 10), GetLineInfo(3, 12), GetLineInfo(3, 13), GetRecentLines(1))
end
function trigger_metadata_status(value)
  return trigger_metadata_result
end
)lua"));

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines        = {engine};
	request.kind           = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName   = QStringLiteral("trigger_metadata_cb");
	request.stringListArg  = {QStringLiteral("trigger"), QStringLiteral("matched argument")};
	request.stringListArg2 = {QStringLiteral("matched argument")};
	const auto styleRuns   = QSharedPointer<QVector<LuaStyleRun>>::create();
	styleRuns->push_back({QStringLiteral("matched argument"), 0xFFFFFF, 0x000000, 0});
	request.styleRunsArg                     = styleRuns;
	request.triggerMatchedLineBufferIndex    = 2;
	request.triggerMatchedLineAbsoluteNumber = 42;
	request.miniWindowSnapshotArg            = snapshot;
	LuaBatchDispatchResult triggerResult     = executor.dispatchBatch(request);
	QVERIFY(!triggerResult.suspended);

	request.kind                        = LuaBatchDispatchKind::StringInOut;
	request.functionName                = QStringLiteral("trigger_metadata_status");
	request.stringArg                   = QStringLiteral("ignored");
	request.miniWindowSnapshotArg       = captureRuntimeCounterDispatchSnapshotForTest(runtime);
	const LuaBatchDispatchResult result = executor.dispatchBatch(request);
	QCOMPARE(result.stringResult, QStringLiteral("true|true|1700000000|42|1.25|2.50|newest presentation"));
}

void tst_LuaCallbackEngine::runtimeTriggerDispatchRepairsStalePresentationIndex()
{
	WorldRuntime            runtime;
	WorldRuntime::LineEntry first;
	first.text       = QStringLiteral("first");
	first.lineNumber = 41;
	WorldRuntime::LineEntry shifted;
	shifted.text       = QStringLiteral("shifted into stale index");
	shifted.lineNumber = 99;
	WorldRuntime::LineEntry matched;
	matched.text       = QStringLiteral("matched");
	matched.lineNumber = 42;
	WorldRuntime::StyleSpan matchedStyle;
	matchedStyle.length     = safeQSizeToInt(matched.text.size());
	matchedStyle.fore       = QColor(QStringLiteral("#123456"));
	matchedStyle.back       = QColor(QStringLiteral("#654321"));
	matchedStyle.underline  = true;
	matchedStyle.changed    = true;
	matchedStyle.actionType = WorldRuntime::ActionHyperlink;
	matchedStyle.action     = QStringLiteral("https://example.invalid/");
	matchedStyle.hint       = QStringLiteral("matched hint");
	matchedStyle.variable   = QStringLiteral("matched variable");
	matchedStyle.startTag   = true;
	matched.spans           = {matchedStyle};
	WorldRuntime::LineEntry newest;
	newest.text       = QStringLiteral("newest");
	newest.lineNumber = 43;
	runtime.replaceOutputLines({first, shifted, matched, newest});

	runtime.setLuaScriptText(QStringLiteral(R"lua(
function repaired_trigger_cb(name, line, wildcards)
  local style = GetStyleInfo(3, 1, 0)
  local fallback = GetStyleInfo(3, 2, 0)
  Note(table.concat({
    "repaired-trigger", GetLineInfo(2, 1), GetLineInfo(3, 1),
    tostring(style.actiontype), style.action, style.hint, style.variable,
    tostring(style.ul), tostring(style.changed), tostring(style.starttag),
    fallback.text, tostring(fallback.length), tostring(fallback.column)
  }, "|"))
end
)lua"));
	QVERIFY(runtime.dispatchLuaResetAndLoadScript(runtime.luaCallbacks()));

	LuaStyleRun callbackStyle;
	callbackStyle.text                          = matched.text;
	callbackStyle.textColour                    = 0x563412;
	callbackStyle.backColour                    = 0x214365;
	callbackStyle.style                         = 0x0002;
	WorldRuntime::StyleSpan partialMatchedStyle = matchedStyle;
	partialMatchedStyle.length                  = 2;
	callbackStyle.sourceSpans.push_back(QMudLuaCallbackLineSnapshot::fromStyleSpan(partialMatchedStyle));
	const QVector<LuaStyleRun>   callbackStyles{callbackStyle};
	const LuaBatchDispatchResult result =
	    runtime.dispatchLuaStringsAndWildcards(runtime.luaCallbacks(), QStringLiteral("repaired_trigger_cb"),
	                                           {QStringLiteral("trigger"), matched.text}, {matched.text}, {},
	                                           &callbackStyles, false, 2, matched.lineNumber);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(result.hasFunction);

	const QString expected = QStringLiteral(
	    "repaired-trigger|shifted into stale index|matched|2|https://example.invalid/|matched hint|matched "
	    "variable|true|true|true|tched|5|3");
	QVERIFY(std::ranges::any_of(runtime.lines(), [&expected](const WorldRuntime::LineEntry &entry)
	                            { return entry.text == expected; }));
}

void tst_LuaCallbackEngine::stringsAndWildcardsDispatchSuppliesSnapshotForCallbackReads()
{
	WorldRuntime      runtime;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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

void tst_LuaCallbackEngine::linePageBaselineCapturesOnlyLastPresentedLine()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);

	QVector<WorldRuntime::LineEntry> presentedLines;
	presentedLines.reserve(runtime.lines().size());
	for (const WorldRuntime::LineEntry &entry : runtime.lines())
		presentedLines.push_back(entry);
	presentedLines.last().lineNumber = 12000;
	runtime.replaceOutputLines(presentedLines);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> entries;
	QStringList                         recentLines;
	const int count = runtime.luaContextLinePageByBufferIndex(0, 0, generation, entries, recentLines);

	QCOMPARE(count, 400);
	QCOMPARE(entries.size(), 1);
	QVERIFY(entries.contains(400));
	QCOMPARE(entries.value(400).text, QStringLiteral("line 400"));
	QCOMPARE(entries.value(400).lineNumber, 12000);
	QVERIFY(recentLines.isEmpty());
}

void tst_LuaCallbackEngine::workerGetLineInfoFetchesBoundedPresentationPages()
{
	WorldRuntime runtime;
	WorldRuntime secondaryRuntime;
	runtime.setWorldAttribute(QStringLiteral("max_output_lines"), QStringLiteral("400"));
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
	QVector<WorldRuntime::LineEntry> presentedLines;
	presentedLines.reserve(runtime.lines().size());
	for (const WorldRuntime::LineEntry &line : runtime.lines())
		presentedLines.push_back(line);
	presentedLines[308].lineNumber = 10000;
	presentedLines[309].lineNumber = 0;
	runtime.replaceOutputLines(presentedLines);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
page_result = ""
function page_cb(name, line, wildcards)
  local first = GetLineInfo(300, 1)
  local adjacent = GetLineInfo(299, 1)
  local upper_edge = GetLineInfo(315, 1)
  local next_page = GetLineInfo(316, 1)
  page_result = table.concat({ first, adjacent, upper_edge, next_page }, "|")
end
function cached_page_cb(name, line, wildcards)
  local secondary = GetWorld("Secondary")
  if secondary then
    secondary:GetWorldID()
  end
  page_result = GetLineInfo(316, 1) or "<nil>"
end
function backward_page_cb(name, line, wildcards)
  page_result = table.concat({
    GetLineInfo(300, 1),
    GetLineInfo(205, 1),
    GetLineInfo(204, 1),
    GetLineInfo(109, 1)
  }, "|")
end
function forward_tail_page_cb(name, line, wildcards)
  page_result = table.concat({
    GetLineInfo(205, 1),
    GetLineInfo(301, 1),
    GetLineInfo(397, 1),
    GetLineInfo(396, 1)
  }, "|")
end
function page_status(value)
  return page_result
end
)lua"),
	                       &runtime);

	quint64                             lineBufferGeneration = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int lineBufferCount = runtime.luaContextLinePageByBufferIndex(0, 0, lineBufferGeneration,
	                                                                    ignoredEntries, ignoredRecentLines);
	auto      lineSnapshot    = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration = lineBufferGeneration;
	lineSnapshot->lineBufferCount      = lineBufferCount;
	auto snapshot                      = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot    = true;
	snapshot->lineBufferCount          = lineSnapshot->lineBufferCount;
	snapshot->lineBufferSnapshot       = lineSnapshot;

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("page_cb");
	request.stringListArg         = {QStringLiteral("page_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult firstPageRequest;
	dispatchWorkerAndWait(executor, request, firstPageRequest);
	QVERIFY(firstPageRequest.suspended);
	QVERIFY(firstPageRequest.hasPendingModalStringRequest);
	QVERIFY(firstPageRequest.pendingModalStringRequest.beforeRuntimeResumeCallback);

	for (int lineNumber = 401; lineNumber <= 410; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
	firstPageRequest.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());

	LuaBatchDispatchRequest firstResume;
	firstResume.engines       = {engine};
	firstResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	firstResume.modalResumeId = firstPageRequest.modalResumeId;
	LuaBatchDispatchResult secondPageRequest;
	dispatchWorkerAndWait(executor, firstResume, secondPageRequest);
	QVERIFY(secondPageRequest.suspended);
	QVERIFY(secondPageRequest.hasPendingModalStringRequest);
	QVERIFY(secondPageRequest.pendingModalStringRequest.beforeRuntimeResumeCallback);

	for (int lineNumber = 411; lineNumber <= 420; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
	secondPageRequest.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());

	LuaBatchDispatchRequest secondResume;
	secondResume.engines       = {engine};
	secondResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	secondResume.modalResumeId = secondPageRequest.modalResumeId;
	LuaBatchDispatchResult completed;
	dispatchWorkerAndWait(executor, secondResume, completed);
	QVERIFY(!completed.suspended);

	LuaBatchDispatchRequest statusRequest;
	statusRequest.engines      = {engine};
	statusRequest.kind         = LuaBatchDispatchKind::StringInOut;
	statusRequest.functionName = QStringLiteral("page_status");
	statusRequest.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, statusRequest, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("line 310|line 309|line 325|line 336"));

	lineBufferGeneration             = 0;
	const int currentLineBufferCount = runtime.luaContextLinePageByBufferIndex(
	    0, 0, lineBufferGeneration, ignoredEntries, ignoredRecentLines);
	auto cachedLineSnapshot                  = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	cachedLineSnapshot->lineBufferGeneration = lineBufferGeneration;
	cachedLineSnapshot->lineBufferCount      = currentLineBufferCount;
	auto cachedSnapshot                      = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	cachedSnapshot->hasLineBufferSnapshot    = true;
	cachedSnapshot->lineBufferCount          = currentLineBufferCount;
	cachedSnapshot->lineBufferSnapshot       = cachedLineSnapshot;
	cachedSnapshot->hasUiSnapshot            = true;
	LuaCallbackWorldRuntimeSnapshot secondaryWorld;
	secondaryWorld.runtime = &secondaryRuntime;
	secondaryWorld.id      = QStringLiteral("secondary-id");
	secondaryWorld.name    = QStringLiteral("Secondary");
	cachedSnapshot->worldRuntimeSnapshot.push_back(secondaryWorld);
	request.miniWindowSnapshotArg = cachedSnapshot;
	request.functionName          = QStringLiteral("cached_page_cb");
	LuaBatchDispatchResult cachedPageResult;
	dispatchWorkerAndWait(executor, request, cachedPageResult);
	int cachedPageRequestCount = 0;
	while (cachedPageResult.suspended)
	{
		++cachedPageRequestCount;
		QVERIFY(cachedPageRequestCount <= 1);
		QVERIFY(cachedPageResult.pendingModalStringRequest.beforeRuntimeResumeCallback);
		cachedPageResult.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest cachedResume;
		cachedResume.engines       = {engine};
		cachedResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		cachedResume.modalResumeId = cachedPageResult.modalResumeId;
		dispatchWorkerAndWait(executor, cachedResume, cachedPageResult);
	}
	QCOMPARE(cachedPageRequestCount, 1);

	dispatchWorkerAndWait(executor, statusRequest, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("line 336"));

	request.functionName = QStringLiteral("backward_page_cb");
	LuaBatchDispatchResult backwardPage;
	dispatchWorkerAndWait(executor, request, backwardPage);
	int backwardPageRequestCount = 0;
	while (backwardPage.suspended)
	{
		++backwardPageRequestCount;
		QVERIFY(backwardPage.pendingModalStringRequest.beforeRuntimeResumeCallback);
		backwardPage.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest backwardResume;
		backwardResume.engines       = {engine};
		backwardResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		backwardResume.modalResumeId = backwardPage.modalResumeId;
		dispatchWorkerAndWait(executor, backwardResume, backwardPage);
	}
	QCOMPARE(backwardPageRequestCount, 2);
	dispatchWorkerAndWait(executor, statusRequest, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("line 320|line 225|line 224|line 129"));

	request.functionName = QStringLiteral("forward_tail_page_cb");
	LuaBatchDispatchResult forwardTailPage;
	dispatchWorkerAndWait(executor, request, forwardTailPage);
	int forwardTailPageRequestCount = 0;
	while (forwardTailPage.suspended)
	{
		++forwardTailPageRequestCount;
		QVERIFY(forwardTailPage.pendingModalStringRequest.beforeRuntimeResumeCallback);
		forwardTailPage.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest forwardTailResume;
		forwardTailResume.engines       = {engine};
		forwardTailResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		forwardTailResume.modalResumeId = forwardTailPage.modalResumeId;
		dispatchWorkerAndWait(executor, forwardTailResume, forwardTailPage);
	}
	QCOMPARE(forwardTailPageRequestCount, 4);
	dispatchWorkerAndWait(executor, statusRequest, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("line 225|line 321|line 417|line 416"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerLinePageRefreshesPresentationCount()
{
	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("max_output_lines"), QStringLiteral("420"));
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
	{
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
		runtime.addRecentLine(QStringLiteral("line %1").arg(lineNumber));
	}

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
growth_result = ""
function growth_cb(name, line, wildcards)
  local before_recent = GetRecentLines(1)
  local original = GetLineInfo(300, 1)
  local appended = GetLineInfo(405, 1)
  local after_recent = GetRecentLines(1)
  growth_result = table.concat({ before_recent, original, appended, after_recent }, "|")
end
function growth_status(value)
  return growth_result
end
)lua"),
	                       &runtime);

	quint64                             lineBufferGeneration = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int lineBufferCount = runtime.luaContextLinePageByBufferIndex(0, 0, lineBufferGeneration,
	                                                                    ignoredEntries, ignoredRecentLines);
	auto      lineSnapshot    = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration = lineBufferGeneration;
	lineSnapshot->lineBufferCount      = lineBufferCount;
	auto snapshot                      = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot    = true;
	snapshot->lineBufferCount          = lineBufferCount;
	snapshot->lineBufferSnapshot       = lineSnapshot;
	snapshot->hasRecentLinesSnapshot   = true;
	snapshot->recentLinesSnapshot      = runtime.recentLines();

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("growth_cb");
	request.stringListArg         = {QStringLiteral("growth_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult firstPageRequest;
	dispatchWorkerAndWait(executor, request, firstPageRequest);
	QVERIFY(firstPageRequest.suspended);
	QVERIFY(firstPageRequest.pendingModalStringRequest.beforeRuntimeResumeCallback);

	for (int lineNumber = 401; lineNumber <= 410; ++lineNumber)
	{
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
		runtime.addRecentLine(QStringLiteral("line %1").arg(lineNumber));
	}
	firstPageRequest.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());

	LuaBatchDispatchRequest firstResume;
	firstResume.engines       = {engine};
	firstResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	firstResume.modalResumeId = firstPageRequest.modalResumeId;
	LuaBatchDispatchResult grownPageRequest;
	dispatchWorkerAndWait(executor, firstResume, grownPageRequest);
	QVERIFY(grownPageRequest.suspended);
	QVERIFY(grownPageRequest.pendingModalStringRequest.beforeRuntimeResumeCallback);
	grownPageRequest.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());

	LuaBatchDispatchRequest secondResume;
	secondResume.engines       = {engine};
	secondResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	secondResume.modalResumeId = grownPageRequest.modalResumeId;
	LuaBatchDispatchResult completed;
	dispatchWorkerAndWait(executor, secondResume, completed);
	QVERIFY(!completed.suspended);

	LuaBatchDispatchRequest statusRequest;
	statusRequest.engines      = {engine};
	statusRequest.kind         = LuaBatchDispatchKind::StringInOut;
	statusRequest.functionName = QStringLiteral("growth_status");
	statusRequest.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, statusRequest, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("line 400|line 300|line 405|line 410"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerLinePageRefreshesAfterCallbackOutputMutation()
{
	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("max_output_lines"), QStringLiteral("400"));
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
	QObject::connect(&runtime, &WorldRuntime::outputRequested, &runtime,
	                 [&runtime](const QString &text, const bool, const bool)
	                 { runtime.addLine(text, WorldRuntime::LineNote); });
	QObject::connect(&runtime, &WorldRuntime::outputStyledRequested, &runtime,
	                 [&runtime](const QString &text, const QVector<WorldRuntime::StyleSpan> &, const bool,
	                            const bool) { runtime.addLine(text, WorldRuntime::LineNote); });

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
mutation_page_result = ""
function mutation_page_cb(name, line, wildcards)
	local before = GetLineInfo(20, 1)
	Note("callback output")
	mutation_page_result = string.format("%s|%.0f|%s|%s|%s", before,
	  GetLinesInBufferCount(), GetLineInfo(20, 1), GetLineInfo(400, 1), GetRecentLines(1))
end
function mutation_page_status(value)
  return mutation_page_result
end
)lua"),
	                       &runtime);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                   = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration  = generation;
	lineSnapshot->lineBufferCount       = count;
	auto snapshot                       = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot     = true;
	snapshot->lineBufferCount           = count;
	snapshot->lineBufferSnapshot        = lineSnapshot;
	snapshot->hasWorldAttributeSnapshot = true;
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("max_output_lines"), QStringLiteral("400"));

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("mutation_page_cb");
	request.stringListArg         = {QStringLiteral("mutation_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult completed;
	dispatchWorkerAndWait(executor, request, completed);
	int pageRequests = 0;
	while (completed.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 6);
		QVERIFY(completed.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(completed);
		completed.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = completed.modalResumeId;
		dispatchWorkerAndWait(executor, resume, completed);
	}
	QCOMPARE(pageRequests, 5);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 400);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("mutation_page_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("line 20|400|line 21|callback output|"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerDirtyLinePageDoesNotClampToStaleCount()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 100; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
	QObject::connect(&runtime, &WorldRuntime::outputStyledRequested, &runtime,
	                 [&runtime](const QString &text, const QVector<WorldRuntime::StyleSpan> &, const bool,
	                            const bool) { runtime.addLine(text, WorldRuntime::LineNote); });

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
dirty_tail_result = ""
function dirty_tail_cb(name, line, wildcards)
  local appended_line = GetLinesInBufferCount() + 1
  Note("appended")
  dirty_tail_result = GetLineInfo(appended_line, 1) or "<nil>"
end
function dirty_tail_status(value)
  return dirty_tail_result
end
)lua"),
	                       &runtime);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                   = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration  = generation;
	lineSnapshot->lineBufferCount       = count;
	auto snapshot                       = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot     = true;
	snapshot->lineBufferCount           = count;
	snapshot->lineBufferSnapshot        = lineSnapshot;
	snapshot->hasWorldAttributeSnapshot = true;
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("max_output_lines"), QStringLiteral("200"));

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("dirty_tail_cb");
	request.stringListArg         = {QStringLiteral("dirty_tail_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 2);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 1);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 101);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("dirty_tail_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("appended"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerPresentationCountConsumersRefreshAfterMultilineOutput()
{
	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("wrap"), QStringLiteral("1"));
	runtime.setWorldAttribute(QStringLiteral("wrap_column"), QStringLiteral("1000"));
	runtime.setWorldAttribute(QStringLiteral("line_spacing"), QStringLiteral("100"));
	runtime.setWorldAttribute(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
	WorldChildWindow window(QStringLiteral("Scroll Test"));
	window.resize(320, 520);
	window.setRuntime(&runtime);
	window.show();
	for (int lineNumber = 1; lineNumber <= 320; ++lineNumber)
		runtime.outputText(QStringLiteral("scroll primer %1").arg(lineNumber), false, true);
	QCoreApplication::processEvents();
	WorldView *const view = window.view();
	QVERIFY(view);
	const QString boundsWindowId = QStringLiteral("scroll_bounds");
	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	const QString scaleAnchorWindowId = QStringLiteral("scroll_bounds_scale_anchor");
	constexpr int kScaleAnchorLeft    = 2000;
	constexpr int kScaleAnchorWidth   = 100;
	QCOMPARE(runtime.windowCreate(scaleAnchorWindowId, kScaleAnchorLeft, 0, kScaleAnchorWidth, 60, 0,
	                              kMiniWindowAbsoluteLocation, QColor(Qt::black),
	                              QStringLiteral("Plugin.Id")),
	         eOK);
	QCOMPARE(runtime.windowShow(scaleAnchorWindowId, true), eOK);
	const QString secondaryScaleAnchorWindowId = QStringLiteral("scroll_bounds_secondary_scale_anchor");
	constexpr int kSecondaryScaleAnchorLeft    = 3000;
	constexpr int kSecondaryScaleAnchorWidth   = 100;
	QCOMPARE(runtime.windowCreate(secondaryScaleAnchorWindowId, kSecondaryScaleAnchorLeft, 0,
	                              kSecondaryScaleAnchorWidth, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	WorldRuntime secondaryRuntime;
	secondaryRuntime.setWorldAttribute(QStringLiteral("id"), QStringLiteral("secondary-id"));
	secondaryRuntime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Secondary"));

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
presentation_count_result = ""
function presentation_count_cb(name, line, wildcards)
  Note(string.rep("wrapped output ", 200))
  local bottom_status = SetScroll(-1, true)
  local bottom_position = GetInfo(296)
  local clamped_status = SetScroll(1000000, true)
  local clamped_position = GetInfo(296)
  Tell("partial output")
  local top_status = SetScroll(0, true)
  local top_position = GetInfo(296)
  local overflow_status = SetScroll(2147483648, true)
  presentation_count_result = string.format("%.0f|%.0f|%.0f|%.0f|%.0f|%.0f|%.0f",
    bottom_status, bottom_position, clamped_status, clamped_position,
    top_status, top_position, overflow_status)
end
function presentation_count_status(value)
  return presentation_count_result
end
output_scroll_refresh_result = ""
function output_scroll_refresh_cb(name, line, wildcards)
  local bottom_status = SetScroll(-1, true)
  ShowInfoBar(false)
  Note(string.rep("post-scroll wrapped output ", 200))
  local refreshed_position = GetInfo(296)
  local frame_value = GetInfo(249)
  local position_after_frame_query = GetInfo(296)
  local line_count = GetLinesInBufferCount()
  output_scroll_refresh_result = string.format("%.0f|%.0f|%.0f|%.0f|%.0f",
    bottom_status, line_count, refreshed_position, frame_value, position_after_frame_query)
end
function output_scroll_refresh_status(value)
  return output_scroll_refresh_result
end
output_scroll_cached_frame_result = ""
function output_scroll_cached_frame_cb(name, line, wildcards)
  local bottom_status = SetScroll(-1, true)
  local previous_position = GetInfo(296)
  local frame_value = GetInfo(249)
  Note(string.rep("cached-frame wrapped output ", 200))
  local refreshed_position = GetInfo(296)
  output_scroll_cached_frame_result = string.format("%.0f|%.0f|%.0f|%.0f",
    bottom_status, previous_position, frame_value, refreshed_position)
end
function output_scroll_cached_frame_status(value)
  return output_scroll_cached_frame_result
end
output_scroll_visibility_result = ""
function output_scroll_visibility_cb(name, line, wildcards)
  local hide_status = SetScroll(-2, false)
  local hidden_wanted = GetInfo(120) and 1 or 0
  local hidden_width = GetInfo(281)
  local hidden_right = GetInfo(292)
  local show_status = SetScroll(-2, true)
  local shown_wanted = GetInfo(120) and 1 or 0
  local shown_width = GetInfo(281)
  local shown_right = GetInfo(292)
  output_scroll_visibility_result = string.format("%.0f|%.0f|%.0f|%.0f|%.0f|%.0f|%.0f|%.0f",
    hide_status, hidden_wanted, hidden_width, hidden_right,
    show_status, shown_wanted, shown_width, shown_right)
end
function output_scroll_visibility_status(value)
  return output_scroll_visibility_result
end
scroll_visibility_cached_result = ""
function scroll_visibility_cached_cb(name, line, wildcards)
  local scroll_status = SetScroll(-2, false)
  scroll_visibility_cached_result = string.format("%.0f|%.0f", scroll_status,
    GetInfo(120) and 1 or 0)
end
function scroll_visibility_cached_status(value)
  return scroll_visibility_cached_result
end
scroll_then_command_result = ""
function scroll_then_command_cb(name, line, wildcards)
  local scroll_status = SetScroll(-2, false)
  local command_status = SetCommand("preserved command")
  scroll_then_command_result = string.format("%.0f|%.0f|%s",
    scroll_status, command_status, GetCommand())
end
function scroll_then_command_status(value)
  return scroll_then_command_result
end
nested_scroll_result = ""
nested_set_scroll_status = -999
function nested_set_scroll()
  nested_set_scroll_status = SetScroll(0, true)
end
function nested_scroll_cb(name, line, wildcards)
  SetScroll(-1, true)
  local code = CallPlugin("Plugin.Id", "nested_set_scroll")
  nested_scroll_result = string.format("%.0f|%.0f|%.0f", code, nested_set_scroll_status, GetInfo(296))
end
function nested_bookmark_only()
  local last = GetLinesInBufferCount()
  Bookmark(last, true)
  Bookmark(last, false)
end
function nested_bookmark_cb(name, line, wildcards)
  SetScroll(-1, true)
  local code = CallPlugin("Plugin.Id", "nested_bookmark_only")
  nested_scroll_result = string.format("%.0f|%.0f", code, GetInfo(296))
end
function nested_read_refreshed_scroll()
  return GetInfo(296)
end
function nested_refreshed_scroll_cb(name, line, wildcards)
  SetScroll(-1, true)
  ShowInfoBar(false)
  Note(string.rep("nested refreshed-scroll output ", 200))
  local caller_position = GetInfo(296)
  local code, target_position = CallPlugin("Plugin.Id", "nested_read_refreshed_scroll")
  nested_scroll_result = string.format("%.0f|%.0f|%.0f", code, caller_position, target_position)
end
function nested_set_scroll_visibility()
  return SetScroll(-2, false)
end
function nested_scroll_visibility_cb(name, line, wildcards)
  SetScroll(-2, true)
  local width_before = GetInfo(281)
  local code, target_status = CallPlugin("Plugin.Id", "nested_set_scroll_visibility")
  local wanted_after = GetInfo(120) and 1 or 0
  local width_after = GetInfo(281)
  nested_scroll_result = string.format("%.0f|%.0f|%.0f|%.0f|%.0f",
    code, target_status, wanted_after, width_before, width_after)
end
scroll_bounds_result = ""
function local_initial_bounds_cb(flags, hotspot_id)
  WindowPosition("scroll_bounds", 2147483647, 0, 0, 2)
  return true
end
function local_scroll_bounds_cb(flags, hotspot_id)
  SetScroll(-2, false)
  WindowPosition("scroll_bounds", 2147483647, 0, 0, 2)
  scroll_bounds_result = string.format("%.0f", WindowInfo("scroll_bounds", 1))
  return true
end
function local_post_show_bounds_cb(flags, hotspot_id)
  SetScroll(-2, false)
  WindowShow("scroll_bounds_secondary_scale_anchor", true)
  WindowPosition("scroll_bounds", 2147483647, 0, 0, 2)
  return true
end
function local_post_ui_bounds_cb(flags, hotspot_id)
  SetScroll(-2, false)
  ShowInfoBar(false)
  WindowPosition("scroll_bounds", 2147483647, 0, 0, 2)
  return true
end
function local_post_command_height_bounds_cb(flags, hotspot_id)
  assert(SetCommandWindowHeight(180) == 0)
  WindowPosition("scroll_bounds", 0, 2147483647, 0, 2)
  return true
end
function local_post_command_text_bounds_cb(flags, hotspot_id)
  assert(SetCommand("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve") == 0)
  WindowPosition("scroll_bounds", 0, 2147483647, 0, 2)
  return true
end
function cross_world_ui_bounds_cb(flags, hotspot_id)
  local secondary = GetWorld("Secondary")
  secondary:ShowInfoBar(true)
  WindowPosition("scroll_bounds", 0, 2147483647, 0, 2)
  return true
end
function cross_world_command_bounds_cb(flags, hotspot_id)
  local secondary = GetWorld("Secondary")
  assert(secondary:SetCommand("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve") == 0)
  WindowPosition("scroll_bounds", 0, 2147483647, 0, 2)
  return true
end
function nested_show_scale_anchor()
  WindowShow("scroll_bounds_secondary_scale_anchor", true)
end
function nested_mutation_bounds_cb(flags, hotspot_id)
  SetScroll(-2, false)
  CallPlugin("Plugin.Id", "nested_show_scale_anchor")
  WindowPosition("scroll_bounds", 2147483647, 0, 0, 2)
  return true
end
function local_scroll_create_bounds_cb(flags, hotspot_id)
  SetScroll(-2, false)
  WindowCreate("scroll_bounds", 2147483647, 0, 100, 60, 0, 2, 0)
  return true
end
function local_scroll_resize_bounds_cb(flags, hotspot_id)
  SetScroll(-2, false)
  WindowResize("scroll_bounds", 2147483647, 60, -1)
  return true
end
function nested_scroll_bounds_cb(flags, hotspot_id)
  SetScroll(-2, true)
  local code = CallPlugin("Plugin.Id", "nested_set_scroll_visibility")
  WindowPosition("scroll_bounds", 2147483647, 0, 0, 2)
  scroll_bounds_result = string.format("%.0f|%.0f", code, WindowInfo("scroll_bounds", 1))
  return true
end
function scroll_bounds_status(value)
  return scroll_bounds_result
end
function nested_scroll_status(value)
  return nested_scroll_result
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("presentation_count_cb");
	request.stringListArg         = {QStringLiteral("presentation_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 5);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(resumeRequests, 3);
	QVERIFY(runtime.luaContextLinesInBufferCount() > 320);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("presentation_count_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList parts = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(parts.size(), 7);
	QCOMPARE(parts.at(0), QStringLiteral("0"));
	QCOMPARE(parts.at(2), QStringLiteral("0"));
	QCOMPARE(parts.at(4), QStringLiteral("0"));
	QCOMPARE(parts.at(6), QString::number(eBadParameter));
	bool      bottomOk  = false;
	bool      clampedOk = false;
	const int bottom    = parts.at(1).toInt(&bottomOk);
	const int clamped   = parts.at(3).toInt(&clampedOk);
	QVERIFY(bottomOk);
	QVERIFY(clampedOk);
	QCOMPARE(clamped, bottom);
	bool      topOk = false;
	const int top   = parts.at(5).toInt(&topOk);
	QVERIFY(topOk);
	QCOMPARE(top, 0);
	QCoreApplication::processEvents();
	const int appliedPosition = view->outputScrollPosition();
	QCOMPARE(appliedPosition, top);
	QVERIFY(bottom > 0);
	const int guessedPosition = qMax(0, runtime.luaContextLinesInBufferCount() * runtime.outputFontHeight() -
	                                        view->outputClientHeight());
	QVERIFY(guessedPosition != bottom);

	view->setInputText(QString(), false);
	request.functionName          = QStringLiteral("scroll_visibility_cached_cb");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	status.functionName = QStringLiteral("scroll_visibility_cached_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0|0"));
	executeDeferredMutations(result);
	QCoreApplication::processEvents();
	QVERIFY(!view->outputScrollBarWanted());
	QCOMPARE(view->setOutputScroll(-2, true), eOK);
	QCoreApplication::processEvents();

	auto partialCommandUiSnapshot = runtime.luaCallbackSnapshotForBridgedCall();
	QVERIFY(partialCommandUiSnapshot);
	auto mutablePartialCommandUiSnapshot =
	    QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(*partialCommandUiSnapshot);
	mutablePartialCommandUiSnapshot->hasCommandUiSnapshot = false;
	QVERIFY(mutablePartialCommandUiSnapshot->commandUiHasView);
	request.functionName          = QStringLiteral("scroll_visibility_cached_cb");
	request.miniWindowSnapshotArg = mutablePartialCommandUiSnapshot;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0|0"));
	executeDeferredMutations(result);
	QCoreApplication::processEvents();
	QVERIFY(!view->outputScrollBarWanted());
	QCOMPARE(view->setOutputScroll(-2, true), eOK);
	QCoreApplication::processEvents();

	request.functionName          = QStringLiteral("scroll_then_command_cb");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	status.functionName = QStringLiteral("scroll_then_command_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0|0|preserved command"));
	executeDeferredMutations(result);
	QCoreApplication::processEvents();
	QCOMPARE(view->inputText(), QStringLiteral("preserved command"));
	view->setInputText(QString(), false);
	QCOMPARE(view->setOutputScroll(-2, true), eOK);
	QCoreApplication::processEvents();

	request.functionName          = QStringLiteral("output_scroll_refresh_cb");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 4);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(resumeRequests, 3);
	status.functionName = QStringLiteral("output_scroll_refresh_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList refreshedParts = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(refreshedParts.size(), 5);
	QCOMPARE(refreshedParts.at(0), QStringLiteral("0"));
	bool      refreshedCountOk = false;
	const int refreshedCount   = refreshedParts.at(1).toInt(&refreshedCountOk);
	QVERIFY(refreshedCountOk);
	QCOMPARE(refreshedCount, runtime.luaContextLinesInBufferCount());
	bool      refreshedPositionOk = false;
	const int refreshedPosition   = refreshedParts.at(2).toInt(&refreshedPositionOk);
	QVERIFY(refreshedPositionOk);
	QVERIFY(refreshedPosition > bottom);
	bool frameValueOk = false;
	static_cast<void>(refreshedParts.at(3).toInt(&frameValueOk));
	QVERIFY(frameValueOk);
	bool      positionAfterFrameQueryOk = false;
	const int positionAfterFrameQuery   = refreshedParts.at(4).toInt(&positionAfterFrameQueryOk);
	QVERIFY(positionAfterFrameQueryOk);
	QCOMPARE(positionAfterFrameQuery, refreshedPosition);
	QCoreApplication::processEvents();
	QCOMPARE(view->outputScrollPosition(), refreshedPosition);

	request.functionName          = QStringLiteral("output_scroll_cached_frame_cb");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 4);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(resumeRequests, 3);
	status.functionName = QStringLiteral("output_scroll_cached_frame_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList cachedFrameParts = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(cachedFrameParts.size(), 4);
	QCOMPARE(cachedFrameParts.at(0), QStringLiteral("0"));
	bool      previousPositionOk = false;
	const int previousPosition   = cachedFrameParts.at(1).toInt(&previousPositionOk);
	QVERIFY(previousPositionOk);
	bool cachedFrameValueOk = false;
	static_cast<void>(cachedFrameParts.at(2).toInt(&cachedFrameValueOk));
	QVERIFY(cachedFrameValueOk);
	bool      cachedFramePositionOk = false;
	const int cachedFramePosition   = cachedFrameParts.at(3).toInt(&cachedFramePositionOk);
	QVERIFY(cachedFramePositionOk);
	QVERIFY(cachedFramePosition > previousPosition);
	QCoreApplication::processEvents();
	QCOMPARE(view->outputScrollPosition(), cachedFramePosition);

	request.functionName          = QStringLiteral("output_scroll_visibility_cb");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 3);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(resumeRequests, 2);
	status.functionName = QStringLiteral("output_scroll_visibility_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList visibilityParts = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(visibilityParts.size(), 8);
	QCOMPARE(visibilityParts.at(0), QStringLiteral("0"));
	QCOMPARE(visibilityParts.at(1), QStringLiteral("0"));
	QCOMPARE(visibilityParts.at(4), QStringLiteral("0"));
	QCOMPARE(visibilityParts.at(5), QStringLiteral("1"));
	bool      hiddenWidthOk = false;
	const int hiddenWidth   = visibilityParts.at(2).toInt(&hiddenWidthOk);
	QVERIFY(hiddenWidthOk);
	bool      hiddenRightOk = false;
	const int hiddenRight   = visibilityParts.at(3).toInt(&hiddenRightOk);
	QVERIFY(hiddenRightOk);
	bool      shownWidthOk = false;
	const int shownWidth   = visibilityParts.at(6).toInt(&shownWidthOk);
	QVERIFY(shownWidthOk);
	bool      shownRightOk = false;
	const int shownRight   = visibilityParts.at(7).toInt(&shownRightOk);
	QVERIFY(shownRightOk);
	QVERIFY(hiddenWidth > shownWidth);
	QVERIFY(hiddenRight > shownRight);
	QCoreApplication::processEvents();
	QVERIFY(view->outputScrollBarWanted());
	QCOMPARE(view->outputClientWidth(), shownWidth);
	QCOMPARE(view->outputTextViewportRectangle().right(), shownRight - 1);

	request.functionName          = QStringLiteral("nested_scroll_cb");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 4);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(resumeRequests, 1);
	status.functionName = QStringLiteral("nested_scroll_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0|0|0"));

	request.functionName          = QStringLiteral("nested_bookmark_cb");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 1);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(resumeRequests, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList nestedBookmarkParts = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(nestedBookmarkParts.size(), 2);
	QCOMPARE(nestedBookmarkParts.at(0), QStringLiteral("0"));
	bool      nestedBookmarkPositionOk = false;
	const int nestedBookmarkPosition   = nestedBookmarkParts.at(1).toInt(&nestedBookmarkPositionOk);
	QVERIFY(nestedBookmarkPositionOk);
	QVERIFY(nestedBookmarkPosition > 0);

	request.functionName          = QStringLiteral("nested_refreshed_scroll_cb");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 3);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(resumeRequests, 1);
	status.functionName = QStringLiteral("nested_scroll_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList nestedRefreshedParts = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(nestedRefreshedParts.size(), 3);
	QCOMPARE(nestedRefreshedParts.at(0), QStringLiteral("0"));
	bool      callerPositionOk = false;
	const int callerPosition   = nestedRefreshedParts.at(1).toInt(&callerPositionOk);
	QVERIFY(callerPositionOk);
	bool      targetPositionOk = false;
	const int targetPosition   = nestedRefreshedParts.at(2).toInt(&targetPositionOk);
	QVERIFY(targetPositionOk);
	QCOMPARE(targetPosition, callerPosition);
	QVERIFY(targetPosition > 0);
	QCoreApplication::processEvents();
	QCOMPARE(view->outputScrollPosition(), targetPosition);

	request.functionName          = QStringLiteral("nested_scroll_visibility_cb");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 4);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(resumeRequests, 2);
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList nestedVisibilityParts = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(nestedVisibilityParts.size(), 5);
	QCOMPARE(nestedVisibilityParts.at(0), QStringLiteral("0"));
	QCOMPARE(nestedVisibilityParts.at(1), QStringLiteral("0"));
	QCOMPARE(nestedVisibilityParts.at(2), QStringLiteral("0"));
	bool      nestedWidthBeforeOk = false;
	const int nestedWidthBefore   = nestedVisibilityParts.at(3).toInt(&nestedWidthBeforeOk);
	QVERIFY(nestedWidthBeforeOk);
	bool      nestedWidthAfterOk = false;
	const int nestedWidthAfter   = nestedVisibilityParts.at(4).toInt(&nestedWidthAfterOk);
	QVERIFY(nestedWidthAfterOk);
	QVERIFY(nestedWidthAfter > nestedWidthBefore);
	QCoreApplication::processEvents();
	QVERIFY(!view->outputScrollBarWanted());
	QCOMPARE(view->outputClientWidth(), nestedWidthAfter);

	QCOMPARE(view->setOutputScroll(-2, true), eOK);
	QCoreApplication::processEvents();
	QCOMPARE(runtime.windowPosition(boundsWindowId, 0, 0, 0, kMiniWindowAbsoluteLocation), eOK);
	const auto unconstrainedGeometryBefore = runtime.luaCallbackSnapshotForBridgedCall();
	QVERIFY(unconstrainedGeometryBefore);
	const MiniWindow *const scaleAnchorBefore = runtime.miniWindow(scaleAnchorWindowId);
	QVERIFY(scaleAnchorBefore);
	const QRect scaleAnchorRectBefore   = scaleAnchorBefore->rect;
	const bool  scaleAnchorHiddenBefore = scaleAnchorBefore->temporarilyHide;
	const WorldRuntime::MiniWindowGeometryConstraintSnapshot initialBoundsGeometry =
	    runtime.miniWindowGeometryConstraintSnapshot();
	const auto unconstrainedGeometryAfter = runtime.luaCallbackSnapshotForBridgedCall();
	QVERIFY(unconstrainedGeometryAfter);
	QCOMPARE(unconstrainedGeometryAfter->absoluteMiniWindowScaleXOver,
	         unconstrainedGeometryBefore->absoluteMiniWindowScaleXOver);
	QCOMPARE(unconstrainedGeometryAfter->absoluteMiniWindowScaleYOver,
	         unconstrainedGeometryBefore->absoluteMiniWindowScaleYOver);
	QCOMPARE(unconstrainedGeometryAfter->absoluteMiniWindowScaleXUnder,
	         unconstrainedGeometryBefore->absoluteMiniWindowScaleXUnder);
	QCOMPARE(unconstrainedGeometryAfter->absoluteMiniWindowScaleYUnder,
	         unconstrainedGeometryBefore->absoluteMiniWindowScaleYUnder);
	const MiniWindow *const scaleAnchorAfter = runtime.miniWindow(scaleAnchorWindowId);
	QVERIFY(scaleAnchorAfter);
	QCOMPARE(scaleAnchorAfter->rect, scaleAnchorRectBefore);
	QCOMPARE(scaleAnchorAfter->temporarilyHide, scaleAnchorHiddenBefore);
	QVERIFY(initialBoundsGeometry.scaleXOver > 0.0);
	QVERIFY(initialBoundsGeometry.scaleXOver < 1.0);
	const int canonicalBoundsWidth = qRound(static_cast<double>(initialBoundsGeometry.displayClientWidth) /
	                                        initialBoundsGeometry.scaleXOver);
	QCOMPARE(canonicalBoundsWidth, kScaleAnchorLeft + kScaleAnchorWidth);

	auto makeBoundsSnapshot = [&runtime, &secondaryRuntime, &boundsWindowId]
	{
		const auto baseline = runtime.luaCallbackSnapshotForBridgedCall(boundsWindowId);
		auto       snapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(*baseline);
		LuaCallbackWorldRuntimeSnapshot secondaryWorld;
		secondaryWorld.runtime = &secondaryRuntime;
		secondaryWorld.id      = QStringLiteral("secondary-id");
		secondaryWorld.name    = QStringLiteral("Secondary");
		snapshot->worldRuntimeSnapshot.push_back(secondaryWorld);
		return snapshot;
	};
	request.kind                    = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.numberArg1              = 0;
	request.stringArg2              = QStringLiteral("scroll_bounds_hotspot");
	request.hasActionSourceOverride = true;
	request.actionSourceOverride    = WorldRuntime::eHotspotCallback;
	auto dispatchBoundsCallback     = [&](const QString &functionName, const int maximumResumeRequests)
	{
		request.functionName          = functionName;
		request.miniWindowSnapshotArg = makeBoundsSnapshot();
		dispatchWorkerAndWait(executor, request, result);
		resumeRequests = 0;
		while (result.suspended)
		{
			++resumeRequests;
			QVERIFY(resumeRequests <= maximumResumeRequests);
			QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
			executeDeferredMutations(result);
			result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
			LuaBatchDispatchRequest resume;
			resume.engines       = {engine};
			resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
			resume.modalResumeId = result.modalResumeId;
			dispatchWorkerAndWait(executor, resume, result);
		}
		executeDeferredMutations(result);
		QCoreApplication::processEvents();
	};

	dispatchBoundsCallback(QStringLiteral("local_initial_bounds_cb"), 1);
	QCOMPARE(resumeRequests, 0);
	const int initialBoundsLeft = canonicalBoundsWidth - 100;
	QCOMPARE(runtime.windowInfo(boundsWindowId, 1).toInt(), initialBoundsLeft);
	QCOMPARE(runtime.windowPosition(boundsWindowId, 0, 0, 0, kMiniWindowAbsoluteLocation), eOK);

	dispatchBoundsCallback(QStringLiteral("local_scroll_bounds_cb"), 2);
	QCOMPARE(resumeRequests, 1);
	const int hiddenBoundsLeft = canonicalBoundsWidth - 100;
	QCOMPARE(runtime.windowInfo(boundsWindowId, 1).toInt(), hiddenBoundsLeft);

	QCOMPARE(view->setOutputScroll(-2, true), eOK);
	QCoreApplication::processEvents();
	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	dispatchBoundsCallback(QStringLiteral("local_scroll_create_bounds_cb"), 2);
	QCOMPARE(resumeRequests, 1);
	QCOMPARE(runtime.windowInfo(boundsWindowId, 1).toInt(), hiddenBoundsLeft);
	QCOMPARE(runtime.windowInfo(boundsWindowId, 3).toInt(), 100);

	QCOMPARE(view->setOutputScroll(-2, true), eOK);
	QCoreApplication::processEvents();
	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	dispatchBoundsCallback(QStringLiteral("local_scroll_resize_bounds_cb"), 2);
	QCOMPARE(resumeRequests, 1);
	QCOMPARE(runtime.windowInfo(boundsWindowId, 1).toInt(), 0);
	QCOMPARE(runtime.windowInfo(boundsWindowId, 3).toInt(), canonicalBoundsWidth);

	QCOMPARE(view->setOutputScroll(-2, true), eOK);
	QCoreApplication::processEvents();
	QCOMPARE(runtime.windowShow(secondaryScaleAnchorWindowId, false), eOK);
	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	dispatchBoundsCallback(QStringLiteral("local_post_show_bounds_cb"), 3);
	QCOMPARE(resumeRequests, 1);
	constexpr int expandedBoundsLeft = kSecondaryScaleAnchorLeft + kSecondaryScaleAnchorWidth - 100;
	QCOMPARE(runtime.windowInfo(boundsWindowId, 1).toInt(), expandedBoundsLeft);

	QCOMPARE(view->setOutputScroll(-2, true), eOK);
	QCoreApplication::processEvents();
	QCOMPARE(runtime.windowShow(secondaryScaleAnchorWindowId, false), eOK);
	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	dispatchBoundsCallback(QStringLiteral("local_post_ui_bounds_cb"), 3);
	QCOMPARE(resumeRequests, 1);
	QCOMPARE(runtime.windowInfo(boundsWindowId, 1).toInt(), hiddenBoundsLeft);

	QCOMPARE(view->setOutputScroll(-2, true), eOK);
	QCoreApplication::processEvents();
	QCOMPARE(runtime.windowShow(secondaryScaleAnchorWindowId, false), eOK);
	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	dispatchBoundsCallback(QStringLiteral("nested_mutation_bounds_cb"), 3);
	QCOMPARE(resumeRequests, 1);
	QCOMPARE(runtime.windowInfo(boundsWindowId, 1).toInt(), expandedBoundsLeft);

	QCOMPARE(view->setOutputScroll(-2, true), eOK);
	QCoreApplication::processEvents();
	QCOMPARE(runtime.windowShow(secondaryScaleAnchorWindowId, false), eOK);
	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	dispatchBoundsCallback(QStringLiteral("nested_scroll_bounds_cb"), 4);
	QCOMPARE(resumeRequests, 1);
	const int nestedBoundsLeft = canonicalBoundsWidth - 100;
	QCOMPARE(runtime.windowInfo(boundsWindowId, 1).toInt(), nestedBoundsLeft);
	status.functionName = QStringLiteral("scroll_bounds_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0|%1").arg(nestedBoundsLeft));

	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	dispatchBoundsCallback(QStringLiteral("local_post_command_height_bounds_cb"), 2);
	QCOMPARE(resumeRequests, 1);
	WorldRuntime::MiniWindowGeometryConstraintSnapshot commandHeightGeometry =
	    runtime.miniWindowGeometryConstraintSnapshot();
	QCOMPARE(runtime.windowInfo(boundsWindowId, 2).toInt(), commandHeightGeometry.displayClientHeight - 60);

	view->setInputText(QString(), false);
	view->synchronizePendingInputViewportLayout();
	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	dispatchBoundsCallback(QStringLiteral("local_post_command_text_bounds_cb"), 2);
	QCOMPARE(resumeRequests, 1);
	const WorldRuntime::MiniWindowGeometryConstraintSnapshot commandTextGeometry =
	    runtime.miniWindowGeometryConstraintSnapshot();
	QCOMPARE(runtime.windowInfo(boundsWindowId, 2).toInt(), commandTextGeometry.displayClientHeight - 60);

	view->setInputText(QString(), false);
	view->synchronizePendingInputViewportLayout();
	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	dispatchBoundsCallback(QStringLiteral("cross_world_ui_bounds_cb"), 3);
	QCOMPARE(resumeRequests, 2);
	const WorldRuntime::MiniWindowGeometryConstraintSnapshot crossWorldGeometry =
	    runtime.miniWindowGeometryConstraintSnapshot();
	QCOMPARE(runtime.windowInfo(boundsWindowId, 2).toInt(), crossWorldGeometry.displayClientHeight - 60);

	view->setInputText(QString(), false);
	view->synchronizePendingInputViewportLayout();
	secondaryRuntime.setView(view);
	QCOMPARE(runtime.windowCreate(boundsWindowId, 0, 0, 100, 60, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QStringLiteral("Plugin.Id")),
	         eOK);
	dispatchBoundsCallback(QStringLiteral("cross_world_command_bounds_cb"), 3);
	QCOMPARE(resumeRequests, 2);
	const WorldRuntime::MiniWindowGeometryConstraintSnapshot crossWorldCommandGeometry =
	    runtime.miniWindowGeometryConstraintSnapshot();
	QCOMPARE(runtime.windowInfo(boundsWindowId, 2).toInt(),
	         crossWorldCommandGeometry.displayClientHeight - 60);
	secondaryRuntime.setView(nullptr);

	teardownWorkerEngine(executor, engine);
	window.setRuntime(nullptr);
}

void tst_LuaCallbackEngine::workerPresentationSnapshotsRefreshFrameDataCoherently()
{
	MainWindow frame;
	frame.resize(900, 700);
	frame.show();
	frame.setInfoBarVisible(true);

	WorldRuntime runtime(&frame);
	runtime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Primary"));
	runtime.setWorldAttribute(QStringLiteral("auto_resize_command_window"), QStringLiteral("1"));
	runtime.setWorldAttribute(QStringLiteral("auto_resize_minimum_lines"), QStringLiteral("1"));
	runtime.setWorldAttribute(QStringLiteral("auto_resize_maximum_lines"), QStringLiteral("20"));
	runtime.setWorldAttribute(QStringLiteral("history_lines"), QStringLiteral("20"));
	auto *window = new WorldChildWindow(QStringLiteral("Presentation Snapshot"));
	window->resize(640, 520);
	window->setRuntime(&runtime);
	frame.addMdiSubWindow(window, true);
	window->show();
	QCoreApplication::processEvents();

	WorldView *const view = window->view();
	QVERIFY(view);
	view->setInputText(QString(), false);
	view->synchronizePendingInputViewportLayout();
	const int singleLineOutputHeight = view->outputClientHeight();
	view->setInputText(QStringLiteral("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight"), false);
	const auto coherentSnapshot = runtime.luaCallbackSnapshotForBridgedCall(QStringLiteral("bounds"));
	QVERIFY(coherentSnapshot);
	QVERIFY(coherentSnapshot->commandUiOutputClientHeight < singleLineOutputHeight);
	QCOMPARE(coherentSnapshot->commandUiOutputClientHeight,
	         coherentSnapshot->geometryConstraintDisplayClientHeight);
	QCOMPARE(coherentSnapshot->commandUiValues.value(QStringLiteral("outputClientHeight")).toInt(),
	         coherentSnapshot->geometryConstraintDisplayClientHeight);
	view->setInputText(QString(), false);
	view->synchronizePendingInputViewportLayout();
	view->appendOutputText(QStringLiteral("hoverword marker"), true);
	QCoreApplication::processEvents();
	bool         cursorOverWord         = false;
	const QPoint originalCursorPosition = QCursor::pos();
	for (int y = 2; y <= 40 && !cursorOverWord; y += 2)
	{
		for (int x = 2; x <= 160; x += 2)
		{
			QCursor::setPos(view->mapToGlobal(QPoint{x, y}));
			if (view->wordUnderCursor() == QStringLiteral("hoverword"))
			{
				cursorOverWord = true;
				break;
			}
		}
	}
	QVERIFY(cursorOverWord);
	runtime.setWordUnderMenu(QString(), false);

	auto              caller      = QSharedPointer<LuaCallbackEngine>::create();
	auto              target      = QSharedPointer<LuaCallbackEngine>::create();
	auto              historyPeer = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, target, QStringLiteral(R"lua(
function dirty_frame_from_call()
  ShowInfoBar(false)
end
function dirty_command_from_call()
  SetCommand("nested command")
end
function dirty_second_command_from_call()
  SetCommand("second command")
end
function push_nested_command()
  return PushCommand()
end
function read_command_history()
  return table.concat(GetCommandList(1) or {}, ",")
end
function delete_nested_history()
  DeleteCommandHistory()
end
function relay_history_after_nested_delete()
  local before = table.concat(GetCommandList(1) or {}, ",")
  local delete_code = CallPlugin("History.Peer", "delete_peer_history")
  local read_code, after = CallPlugin("History.Peer", "read_peer_history")
  return string.format("%s|%.0f|%.0f|%s", before, delete_code, read_code, after or "<nil>")
end
function OnPluginBroadcast(message, sender_id, sender_name, text)
  ShowInfoBar(true)
end
)lua"),
	                       &runtime);
	initializeWorkerEngine(executor, historyPeer, QStringLiteral(R"lua(
function delete_peer_history()
  DeleteCommandHistory()
end
function read_peer_history()
  return table.concat(GetCommandList(1) or {}, ",")
end
)lua"),
	                       &runtime, QStringLiteral("History.Peer"));
	initializeWorkerEngine(executor, caller, QStringLiteral(R"lua(
frame_refresh_result = ""
selected_refresh_result = ""
history_refresh_result = ""
history_overlay_result = ""
nested_push_result = ""
history_delete_result = ""
history_dirty_ui_overlay_result = ""
history_nested_relay_result = ""
self_proxy_history_result = ""
self_proxy_option_result = ""
function frame_refresh_cb(name, line, wildcards)
  local command_code = CallPlugin("Target.Id", "dirty_command_from_call")
  local command_after_call = GetCommand()
  local before = GetInfo(249)
  local code = CallPlugin("Target.Id", "dirty_frame_from_call")
  local selected_after_call = GetInfo(86)
  local output_after_call = GetInfo(280)
  local after_call = GetInfo(249)
  local delivered = BroadcastPlugin(42, "refresh frame")
  local after_broadcast = GetInfo(249)
  frame_refresh_result = string.format("%.0f|%s|%.0f|%.0f|%s|%.0f|%.0f|%.0f|%.0f",
    command_code, command_after_call, before, code, selected_after_call, output_after_call, after_call,
    delivered, after_broadcast)
end
function frame_refresh_status(value)
  return frame_refresh_result
end
function selected_refresh_cb(name, line, wildcards)
  selected_refresh_result = GetInfo(86)
end
function selected_refresh_status(value)
  return selected_refresh_result
end
function history_refresh_cb(name, line, wildcards)
  local code = CallPlugin("Target.Id", "push_nested_command")
  local history = GetCommandList(1)
  history_refresh_result = string.format("%.0f|%s", code, table.concat(history or {}, ","))
end
function history_refresh_status(value)
  return history_refresh_result
end
function history_overlay_cb(name, line, wildcards)
  local before = GetCommandList(1)
  Note("history overlay refresh")
  GetInfo(280)
  local code, target_history = CallPlugin("Target.Id", "read_command_history")
  history_overlay_result = string.format("%s|%.0f|%s", table.concat(before or {}, ","), code,
    target_history or "<nil>")
end
function history_overlay_status(value)
  return history_overlay_result
end
function nested_push_cb(name, line, wildcards)
  local code = CallPlugin("Target.Id", "dirty_second_command_from_call")
  local pushed = PushCommand()
  local history = GetCommandList(1)
  nested_push_result = string.format("%.0f|%s|%s", code, pushed,
    table.concat(history or {}, ","))
end
function nested_push_status(value)
  return nested_push_result
end
function history_delete_cb(name, line, wildcards)
  local before = GetCommandList(1)
  local code = CallPlugin("Target.Id", "delete_nested_history")
  local after = GetCommandList(1)
  history_delete_result = string.format("%s|%.0f|%s", table.concat(before or {}, ","), code,
    table.concat(after or {}, ","))
end
function history_delete_status(value)
  return history_delete_result
end
function history_dirty_ui_overlay_cb(name, line, wildcards)
  local before = table.concat(GetCommandList(1) or {}, ",")
  SetInputFont("Monospace", 10, 400, 0)
  local code, target_history = CallPlugin("Target.Id", "read_command_history")
  history_dirty_ui_overlay_result = string.format("%s|%.0f|%s", before, code,
    target_history or "<nil>")
end
function history_dirty_ui_overlay_status(value)
  return history_dirty_ui_overlay_result
end
function history_nested_relay_cb(name, line, wildcards)
  local before = table.concat(GetCommandList(1) or {}, ",")
  local code, relayed = CallPlugin("Target.Id", "relay_history_after_nested_delete")
  history_nested_relay_result = string.format("%s|%.0f|%s", before, code, relayed or "<nil>")
end
function history_nested_relay_status(value)
  return history_nested_relay_result
end
function self_proxy_history_cb(name, line, wildcards)
  local before = table.concat(GetCommandList(1) or {}, ",")
  local current = GetWorld("Primary")
  current:DeleteCommandHistory()
  local after = table.concat(GetCommandList(1) or {}, ",")
  self_proxy_history_result = before .. "|" .. after
end
function self_proxy_history_status(value)
  return self_proxy_history_result
end
function self_proxy_option_cb(name, line, wildcards)
  SetAlphaOption("script_prefix", "before")
  local before = GetAlphaOption("script_prefix")
  local current = GetWorld("Primary")
  local code = current:SetAlphaOption("script_prefix", "after")
  local after = GetAlphaOption("script_prefix")
  self_proxy_option_result = string.format("%s|%.0f|%s", before, code, after)
end
function self_proxy_option_status(value)
  return self_proxy_option_result
end
)lua"),
	                       &runtime);

	const QString targetKey               = QStringLiteral("target.id");
	const QString peerKey                 = QStringLiteral("history.peer");
	const auto    captureDispatchSnapshot = [&runtime, &target, &historyPeer, &targetKey, &peerKey]
	{
		const auto baseSnapshot = runtime.luaCallbackSnapshotForBridgedCall();
		if (!baseSnapshot)
			return QSharedPointer<LuaCallbackMiniWindowSnapshot>{};
		auto snapshot               = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(*baseSnapshot);
		snapshot->pluginIdsSnapshot = {targetKey, peerKey};
		snapshot->pluginIdsByLookupKey.insert(targetKey, targetKey);
		snapshot->pluginIdsByLookupKey.insert(peerKey, peerKey);
		snapshot->pluginNamesById.insert(targetKey, QStringLiteral("Target Plugin"));
		snapshot->pluginNamesById.insert(peerKey, QStringLiteral("History Peer"));
		snapshot->pluginEnabledById.insert(targetKey, true);
		snapshot->pluginEnabledById.insert(peerKey, true);
		snapshot->pluginEnginesById.insert(targetKey, target);
		snapshot->pluginEnginesById.insert(peerKey, historyPeer);
		snapshot->hasBroadcastPluginSnapshot     = true;
		snapshot->broadcastPluginIdsSnapshot     = {targetKey};
		snapshot->broadcastPluginEnginesSnapshot = {target};
		return snapshot;
	};
	const auto snapshot = captureDispatchSnapshot();
	QVERIFY(snapshot);

	LuaBatchDispatchRequest request;
	request.engines               = {caller};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("frame_refresh_cb");
	request.stringListArg         = {QStringLiteral("frame_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 4);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(resumeRequests, 4);
	QVERIFY(result.commandUiPresentationRequiresRefresh);
	QVERIFY(result.globalPresentationRequiresRefresh);

	LuaBatchDispatchRequest status;
	status.engines      = {caller};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("frame_refresh_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList values = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(values.size(), 9);
	QCOMPARE(values.at(0), QStringLiteral("0"));
	QCOMPARE(values.at(1), QStringLiteral("nested command"));
	QCOMPARE(values.at(3), QStringLiteral("0"));
	QCOMPARE(values.at(4), QStringLiteral("hoverword"));
	QCOMPARE(values.at(7), QStringLiteral("1"));
	for (const int index : {2, 5, 6, 8})
	{
		bool ok = false;
		QVERIFY(values.at(index).toInt(&ok) > 0);
		QVERIFY(ok);
	}

	runtime.setWordUnderMenu(QString(), false);
	request.functionName          = QStringLiteral("selected_refresh_cb");
	request.miniWindowSnapshotArg = captureDispatchSnapshot();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 1);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(resumeRequests, 1);
	status.functionName = QStringLiteral("selected_refresh_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("hoverword"));

	request.functionName          = QStringLiteral("history_refresh_cb");
	request.miniWindowSnapshotArg = captureDispatchSnapshot();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 2);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	executeDeferredMutations(result);
	QCOMPARE(resumeRequests, 2);
	status.functionName = QStringLiteral("history_refresh_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0|nested command"));
	QVERIFY(result.commandHistoryChanged);

	request.functionName          = QStringLiteral("history_overlay_cb");
	request.miniWindowSnapshotArg = captureDispatchSnapshot();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 2);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	executeDeferredMutations(result);
	QCOMPARE(resumeRequests, 2);
	status.functionName = QStringLiteral("history_overlay_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("nested command|0|nested command"));
	QCOMPARE(view->commandHistoryList(), QStringList{QStringLiteral("nested command")});

	request.functionName          = QStringLiteral("nested_push_cb");
	request.miniWindowSnapshotArg = captureDispatchSnapshot();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 2);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	executeDeferredMutations(result);
	QCOMPARE(resumeRequests, 1);
	status.functionName = QStringLiteral("nested_push_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0|second command|second command"));
	const QStringList expectedHistory = {QStringLiteral("nested command"), QStringLiteral("second command")};
	QCOMPARE(view->commandHistoryList(), expectedHistory);

	request.functionName          = QStringLiteral("history_delete_cb");
	request.miniWindowSnapshotArg = captureDispatchSnapshot();
	dispatchWorkerAndWait(executor, request, result);
	resumeRequests = 0;
	while (result.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 2);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	executeDeferredMutations(result);
	QCOMPARE(resumeRequests, 2);
	QVERIFY(result.commandHistoryChanged);
	status.functionName = QStringLiteral("history_delete_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("second command|0|"));
	QVERIFY(view->commandHistoryList().isEmpty());

	view->setInputText(QStringLiteral("repeat command"), false);
	QCOMPARE(view->pushCommand(), QStringLiteral("repeat command"));
	view->clearCommandHistory();
	view->setInputText(QStringLiteral("repeat command"), false);
	QCOMPARE(view->pushCommand(), QStringLiteral("repeat command"));
	QCOMPARE(view->commandHistoryList(), QStringList{QStringLiteral("repeat command")});

	const auto runHistoryCallback =
	    [&](const QString &functionName, const int maximumResumeRequests, int &callbackResumeRequests)
	{
		request.functionName          = functionName;
		request.miniWindowSnapshotArg = captureDispatchSnapshot();
		dispatchWorkerAndWait(executor, request, result);
		callbackResumeRequests = 0;
		while (result.suspended)
		{
			++callbackResumeRequests;
			QVERIFY(callbackResumeRequests <= maximumResumeRequests);
			QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
			executeDeferredMutations(result);
			result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
			LuaBatchDispatchRequest resume;
			resume.engines       = {caller};
			resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
			resume.modalResumeId = result.modalResumeId;
			dispatchWorkerAndWait(executor, resume, result);
		}
		executeDeferredMutations(result);
	};

	int callbackResumeRequests = 0;
	runHistoryCallback(QStringLiteral("history_dirty_ui_overlay_cb"), 1, callbackResumeRequests);
	QCOMPARE(callbackResumeRequests, 1);
	status.functionName = QStringLiteral("history_dirty_ui_overlay_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("repeat command|0|repeat command"));
	QCOMPARE(view->commandHistoryList(), QStringList{QStringLiteral("repeat command")});

	runHistoryCallback(QStringLiteral("history_nested_relay_cb"), 2, callbackResumeRequests);
	QCOMPARE(callbackResumeRequests, 2);
	status.functionName = QStringLiteral("history_nested_relay_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("repeat command|0|repeat command|0|0|"));
	QVERIFY(view->commandHistoryList().isEmpty());

	view->setInputText(QStringLiteral("repeat command"), false);
	QCOMPARE(view->pushCommand(), QStringLiteral("repeat command"));
	runHistoryCallback(QStringLiteral("self_proxy_history_cb"), 1, callbackResumeRequests);
	QCOMPARE(callbackResumeRequests, 1);
	status.functionName = QStringLiteral("self_proxy_history_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("repeat command|"));
	QVERIFY(view->commandHistoryList().isEmpty());

	runHistoryCallback(QStringLiteral("self_proxy_option_cb"), 0, callbackResumeRequests);
	QCOMPARE(callbackResumeRequests, 0);
	status.functionName = QStringLiteral("self_proxy_option_status");
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("before|0|after"));
	QCOMPARE(runtime.worldAttributeValue(QStringLiteral("script_prefix")), QStringLiteral("after"));
	QCursor::setPos(originalCursorPosition);

	teardownWorkerEngine(executor, caller);
	teardownWorkerEngine(executor, target);
	teardownWorkerEngine(executor, historyPeer);
	window->setRuntime(nullptr);
}

void tst_LuaCallbackEngine::numberAndStringResumeKeepsPresentationFlags()
{
	LuaBatchDispatchResult accumulated;
	accumulated.linePresentationRequiresRefresh      = true;
	accumulated.outputScrollPositionRequiresRefresh  = true;
	accumulated.outputScrollPositionChanged          = true;
	accumulated.commandUiPresentationRequiresRefresh = true;
	accumulated.globalPresentationRequiresRefresh    = true;
	accumulated.commandHistoryChanged                = true;
	accumulated.notepadPresentationChanged           = true;
	accumulated.hasNotepadPresentationSnapshot       = true;
	LuaCallbackNotepadSnapshot originalNotepad;
	originalNotepad.title = QStringLiteral("original");
	accumulated.notepadPresentationSnapshot.push_back(originalNotepad);
	LuaBatchDispatchResult laterRecipient{};
	LuaBatchDispatchResult finalResult = laterRecipient;
	mergeLuaBatchPresentationRefreshFlags(finalResult, accumulated);
	QVERIFY(finalResult.linePresentationRequiresRefresh);
	QVERIFY(finalResult.outputScrollPositionRequiresRefresh);
	QVERIFY(finalResult.outputScrollPositionChanged);
	QVERIFY(finalResult.commandUiPresentationRequiresRefresh);
	QVERIFY(finalResult.globalPresentationRequiresRefresh);
	QVERIFY(finalResult.commandHistoryChanged);
	QVERIFY(finalResult.notepadPresentationChanged);
	QVERIFY(finalResult.hasNotepadPresentationSnapshot);
	QCOMPARE(finalResult.notepadPresentationSnapshot.size(), 1);
	QCOMPARE(finalResult.notepadPresentationSnapshot.constFirst().title, QStringLiteral("original"));

	LuaBatchDispatchResult unavailablePresentation;
	unavailablePresentation.notepadPresentationChanged     = true;
	unavailablePresentation.hasNotepadPresentationSnapshot = false;
	mergeLuaBatchPresentationRefreshFlags(finalResult, unavailablePresentation);
	QVERIFY(finalResult.notepadPresentationChanged);
	QVERIFY(!finalResult.hasNotepadPresentationSnapshot);
	QVERIFY(finalResult.notepadPresentationSnapshot.isEmpty());

	LuaBatchDispatchResult validEmptyPresentation;
	validEmptyPresentation.notepadPresentationChanged     = true;
	validEmptyPresentation.hasNotepadPresentationSnapshot = true;
	mergeLuaBatchPresentationRefreshFlags(finalResult, validEmptyPresentation);
	QVERIFY(finalResult.hasNotepadPresentationSnapshot);
	QVERIFY(finalResult.notepadPresentationSnapshot.isEmpty());

	LuaBatchDispatchResult newestPresentation;
	newestPresentation.notepadPresentationChanged     = true;
	newestPresentation.hasNotepadPresentationSnapshot = true;
	LuaCallbackNotepadSnapshot newestNotepad;
	newestNotepad.title = QStringLiteral("newest");
	newestPresentation.notepadPresentationSnapshot.push_back(newestNotepad);
	mergeLuaBatchPresentationRefreshFlags(finalResult, newestPresentation);
	QVERIFY(finalResult.hasNotepadPresentationSnapshot);
	QCOMPARE(finalResult.notepadPresentationSnapshot.size(), 1);
	QCOMPARE(finalResult.notepadPresentationSnapshot.constFirst().title, QStringLiteral("newest"));
}

void tst_LuaCallbackEngine::workerExtremeLineNumbersDoNotOverflowPageBounds()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 10; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
	QObject::connect(&runtime, &WorldRuntime::outputRequested, &runtime,
	                 [&runtime](const QString &text, const bool hardReturn, const bool)
	                 { runtime.addLine(text, WorldRuntime::LineNote, hardReturn); });
	QObject::connect(&runtime, &WorldRuntime::outputStyledRequested, &runtime,
	                 [&runtime](const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
	                            const bool hardReturn, const bool)
	                 { runtime.addLine(text, WorldRuntime::LineNote, spans, hardReturn); });

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
extreme_line_result = ""
function extreme_line_cb(name, line, wildcards)
  Note("dirty")
  local max_line = GetLineInfo(2147483647, 1)
  local oversized_line = GetLineInfo(2147483648, 1)
  local max_style = GetStyleInfo(2147483647, 1, 1)
  extreme_line_result = string.format("%s|%s|%s",
    tostring(max_line == nil), tostring(oversized_line == nil), tostring(max_style == nil))
end
function extreme_line_status(value)
  return extreme_line_result
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("extreme_line_cb");
	request.stringListArg         = {QStringLiteral("extreme_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 2);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 1);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 11);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("extreme_line_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("true|true|true"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerBookmarkUpdatesCachedLineState()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 10; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
bookmark_result = ""
function bookmark_cb(name, line, wildcards)
  local last = GetLinesInBufferCount()
  local before = GetLineInfo(last, 7)
  Bookmark(last, true)
  local after_set = GetLineInfo(last, 7)
  Bookmark(last, false)
  local after_clear = GetLineInfo(last, 7)
  bookmark_result = table.concat({
    tostring(before), tostring(after_set), tostring(after_clear)
  }, "|")
end
function bookmark_status(value)
  return bookmark_result
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("bookmark_cb");
	request.stringListArg         = {QStringLiteral("bookmark_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 3);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 2);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("bookmark_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("false|true|false"));

	WorldRuntime::LineEntry lastEntry;
	QVERIFY(runtime.luaContextLineEntry(10, lastEntry));
	QVERIFY((lastEntry.flags & WorldRuntime::LineBookmark) == 0);

	runtime.setSessionStateOutputBufferSealed(true);
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 3);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 2);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("false|false|false"));
	QVERIFY(runtime.luaContextLineEntry(10, lastEntry));
	QVERIFY((lastEntry.flags & WorldRuntime::LineBookmark) == 0);
	runtime.setSessionStateOutputBufferSealed(false);

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerEmptyDeleteLinesDoesNotRefreshPresentation()
{
	WorldRuntime      runtime;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
empty_delete_result = -1
function empty_delete_cb(name, line, wildcards)
  DeleteLines(1)
  DeleteLines(2147483648)
  empty_delete_result = GetLinesInBufferCount()
end
function empty_delete_status(value)
  return tostring(empty_delete_result)
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("empty_delete_cb");
	request.stringListArg         = {QStringLiteral("empty_delete_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 0);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("empty_delete_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0.0"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::outputRemovalReconcilesActiveIncomingLineIdentity()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("history"), WorldRuntime::LineOutput);
	runtime.beginIncomingLineLuaContext(QStringLiteral("active"), WorldRuntime::LineOutput, {});
	QVERIFY(runtime.reserveIncomingLineLuaContextInBuffer());
	const qint64 activeLineNumber = runtime.incomingLineLuaContextAbsoluteNumber();
	QVERIFY(activeLineNumber > 0);

	runtime.deleteLines(1);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 1);
	WorldRuntime::LineEntry entry;
	QVERIFY(!runtime.luaContextLineEntryByAbsoluteNumber(activeLineNumber, entry));
	QVERIFY(runtime.luaContextLineEntry(1, entry));
	QCOMPARE(entry.text, QStringLiteral("history"));
	runtime.endIncomingLineLuaContext();

	runtime.beginIncomingLineLuaContext(QStringLiteral("active clear"), WorldRuntime::LineOutput, {});
	QVERIFY(runtime.reserveIncomingLineLuaContextInBuffer());
	const qint64 clearedLineNumber = runtime.incomingLineLuaContextAbsoluteNumber();
	runtime.deleteOutput();
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 0);
	QVERIFY(!runtime.luaContextLineEntry(1, entry));
	QVERIFY(!runtime.luaContextLineEntryByAbsoluteNumber(clearedLineNumber, entry));
	runtime.endIncomingLineLuaContext();

	runtime.setWorldAttribute(QStringLiteral("max_output_lines"), QStringLiteral("1"));
	runtime.beginIncomingLineLuaContext(QStringLiteral("evicted active"), WorldRuntime::LineOutput, {});
	QVERIFY(runtime.reserveIncomingLineLuaContextInBuffer());
	const qint64 evictedLineNumber = runtime.incomingLineLuaContextAbsoluteNumber();
	runtime.addLine(QStringLiteral("new tail"), WorldRuntime::LineOutput);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 1);
	QVERIFY(!runtime.luaContextLineEntryByAbsoluteNumber(evictedLineNumber, entry));
	QVERIFY(runtime.luaContextLineEntry(1, entry));
	QCOMPARE(entry.text, QStringLiteral("new tail"));
	runtime.endIncomingLineLuaContext();
}

void tst_LuaCallbackEngine::bufferedReplacementPageUsesLiveRuntimeEntry()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("history"), WorldRuntime::LineOutput);
	runtime.beginIncomingLineLuaContext(QStringLiteral("original trigger"), WorldRuntime::LineOutput, {});
	QVERIFY(runtime.reserveIncomingLineLuaContextInBuffer());
	QVERIFY(runtime.hideBufferedIncomingLineLuaContextForReplacement());
	const qint64 anchorLineNumber = runtime.incomingLineLuaContextAbsoluteNumber();

	QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(
	    anchorLineNumber, 0, true, QStringLiteral("replacement"), WorldRuntime::LineNote, {}, true));
	runtime.bookmarkLine(2, true);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> entries;
	QStringList                         recentLines;
	QCOMPARE(runtime.luaContextLinePageByBufferIndex(2, 2, generation, entries, recentLines), 2);
	QCOMPARE(entries.size(), 1);
	QCOMPARE(entries.value(2).text, QStringLiteral("replacement"));
	QVERIFY((entries.value(2).flags & WorldRuntime::LineHidden) == 0);
	QVERIFY((entries.value(2).flags & WorldRuntime::LineBookmark) != 0);

	WorldRuntime::LineEntry absoluteEntry;
	QVERIFY(runtime.luaContextLineEntryByAbsoluteNumber(anchorLineNumber, absoluteEntry));
	QCOMPARE(absoluteEntry.text, QStringLiteral("replacement"));
	QVERIFY((absoluteEntry.flags & WorldRuntime::LineBookmark) != 0);
	QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(
	    anchorLineNumber, 1, false, QStringLiteral("following"), WorldRuntime::LineNote, {}, true));
	QCOMPARE(runtime.incomingLineLuaContextBufferIndex(), 2);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 3);
	const auto callbackSnapshot = runtime.luaCallbackSnapshotForBridgedCall();
	QVERIFY(callbackSnapshot);
	QCOMPARE(callbackSnapshot->callbackOutputAnchorBufferIndex, 2);
	QCOMPARE(callbackSnapshot->callbackOutputAnchorAbsoluteNumber, anchorLineNumber);
	QCOMPARE(runtime.luaContextLinePageByBufferIndex(3, 3, generation, entries, recentLines), 3);
	QCOMPARE(entries.value(3).text, QStringLiteral("following"));

	runtime.endIncomingLineLuaContext();
	QCOMPARE(runtime.lines().size(), 3);
	QCOMPARE(runtime.lines().constLast().text, QStringLiteral("following"));
}

void tst_LuaCallbackEngine::unusedHiddenReplacementAnchorIsRemovedAtContextEnd()
{
	WorldRuntime runtime;
	runtime.beginIncomingLineLuaContext(QStringLiteral("omitted trigger"), WorldRuntime::LineOutput, {});
	QVERIFY(runtime.reserveIncomingLineLuaContextInBuffer());
	QVERIFY(runtime.hideBufferedIncomingLineLuaContextForReplacement());
	const qint64 hiddenLineNumber = runtime.incomingLineLuaContextAbsoluteNumber();
	QCOMPARE(runtime.lines().size(), 1);
	QVERIFY((runtime.lines().constFirst().flags & WorldRuntime::LineHidden) != 0);

	runtime.endIncomingLineLuaContext();
	QVERIFY(runtime.lines().isEmpty());
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 0);
	WorldRuntime::LineEntry entry;
	QVERIFY(!runtime.luaContextLineEntryByAbsoluteNumber(hiddenLineNumber, entry));
}

void tst_LuaCallbackEngine::anchoredOutputPositioningAvoidsRepeatedFullScans()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);
	const qint64  anchorLineNumber = runtime.lines().constFirst().lineNumber;
	const quint64 scansBefore      = runtime.luaCallbackOutputPositionFullScanCount();
	constexpr int outputLineCount  = 512;
	for (int index = 0; index < outputLineCount; ++index)
	{
		QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(anchorLineNumber, index + 1, false,
		                                                   QStringLiteral("line %1").arg(index + 1),
		                                                   WorldRuntime::LineNote, {}, true));
	}
	QCOMPARE(runtime.luaCallbackOutputPositionFullScanCount() - scansBefore, quint64{1});
	QCOMPARE(runtime.lines().size(), outputLineCount + 1);
	QCOMPARE(runtime.lines().at(1).text, QStringLiteral("line 1"));
	QCOMPARE(runtime.lines().constLast().text, QStringLiteral("line 512"));

	WorldRuntime zeroOffsetRuntime;
	zeroOffsetRuntime.addLine(QStringLiteral("zero-offset anchor"), WorldRuntime::LineOutput);
	const qint64  zeroOffsetAnchor = zeroOffsetRuntime.lines().constFirst().lineNumber;
	const quint64 zeroOffsetScans  = zeroOffsetRuntime.luaCallbackOutputPositionFullScanCount();
	for (int index = 0; index < 64; ++index)
	{
		QVERIFY(zeroOffsetRuntime.writeLuaCallbackOutputAtLineAnchor(zeroOffsetAnchor, 0, false,
		                                                             QStringLiteral("prefix %1").arg(index),
		                                                             WorldRuntime::LineNote, {}, true));
	}
	QCOMPARE(zeroOffsetRuntime.luaCallbackOutputPositionFullScanCount() - zeroOffsetScans, quint64{1});
}

void tst_LuaCallbackEngine::anchoredOutputCursorSurvivesHeadEvictionWithoutRescan()
{
	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("max_output_lines"), QStringLiteral("100"));
	for (int lineNumber = 1; lineNumber <= 99; ++lineNumber)
		runtime.addLine(QStringLiteral("history %1").arg(lineNumber), WorldRuntime::LineOutput);

	const qint64 anchorLineNumber = runtime.lines().constLast().lineNumber;
	QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(anchorLineNumber, 1, false, QStringLiteral("first"),
	                                                   WorldRuntime::LineNote, {}, true));
	const quint64 scansAfterFirstOutput = runtime.luaCallbackOutputPositionFullScanCount();
	for (int lineNumber = 1; lineNumber <= 32; ++lineNumber)
		runtime.addLine(QStringLiteral("new %1").arg(lineNumber), WorldRuntime::LineOutput);

	QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(anchorLineNumber, 2, false, QStringLiteral("second"),
	                                                   WorldRuntime::LineNote, {}, true));
	QCOMPARE(runtime.luaCallbackOutputPositionFullScanCount(), scansAfterFirstOutput);
	int anchorIndex = -1;
	for (int index = 0; index < runtime.lines().size(); ++index)
	{
		if (runtime.lines().at(index).lineNumber == anchorLineNumber)
		{
			anchorIndex = index;
			break;
		}
	}
	QVERIFY(anchorIndex >= 0);
	QCOMPARE(runtime.lines().at(anchorIndex + 1).text, QStringLiteral("first"));
	QCOMPARE(runtime.lines().at(anchorIndex + 2).text, QStringLiteral("second"));
}

void tst_LuaCallbackEngine::anchoredInsertionCursorFollowsStableLineIdentity()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);
	const qint64      anchorLineNumber = runtime.lines().constFirst().lineNumber;
	const quint64     scansBefore      = runtime.luaCallbackOutputPositionFullScanCount();
	constexpr quint64 outerStreamId    = 101;
	constexpr quint64 nestedStreamId   = 202;
	QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(anchorLineNumber, 1, false, QStringLiteral("first"),
	                                                   WorldRuntime::LineNote, {}, true, outerStreamId));
	const qint64 firstLineNumber = runtime.lines().at(1).lineNumber;
	QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(anchorLineNumber, 2, false, QStringLiteral("second"),
	                                                   WorldRuntime::LineNote, {}, true, outerStreamId));
	QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(firstLineNumber, 1, false, QStringLiteral("nested"),
	                                                   WorldRuntime::LineNote, {}, true, nestedStreamId));
	QVERIFY(runtime.writeLuaCallbackOutputAtLineAnchor(anchorLineNumber, 3, false, QStringLiteral("after"),
	                                                   WorldRuntime::LineNote, {}, true, outerStreamId));

	QCOMPARE(logicalOutputLinesFromEntries(runtime.lines()),
	         QStringList({QStringLiteral("anchor"), QStringLiteral("first"), QStringLiteral("nested"),
	                      QStringLiteral("second"), QStringLiteral("after")}));
	QCOMPARE(runtime.luaCallbackOutputPositionFullScanCount() - scansBefore, quint64{2});
	QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{2});
	runtime.releaseLuaCallbackOutputStream(nestedStreamId);
	QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{1});
	runtime.releaseLuaCallbackOutputStream(outerStreamId);
	QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{0});
}

void tst_LuaCallbackEngine::workerAcceptedDeletionPageDoesNotRestoreActiveLine()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 4; ++lineNumber)
		runtime.addLine(QStringLiteral("history %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
accepted_delete_result = ""
function accepted_delete_tail(name, line, wildcards)
  DeleteLines(1)
  local first_line = tostring(GetLineInfo(1, 1))
  accepted_delete_result = string.format("%.0f|%s", GetLinesInBufferCount(), first_line)
end
function accepted_delete_all(name, line, wildcards)
  DeleteOutput()
  local first_line = tostring(GetLineInfo(1, 1))
  accepted_delete_result = string.format("%.0f|%s", GetLinesInBufferCount(), first_line)
end
function accepted_delete_status(value)
  return accepted_delete_result
end
)lua"),
	                       &runtime);

	auto dispatchDeletion = [&](const QString &functionName, const QString &expected)
	{
		runtime.beginIncomingLineLuaContext(QStringLiteral("active trigger"), WorldRuntime::LineOutput, {});
		QVERIFY(runtime.reserveIncomingLineLuaContextInBuffer());
		const qint64            deletedLineNumber = runtime.incomingLineLuaContextAbsoluteNumber();

		LuaBatchDispatchRequest request;
		request.engines               = {engine};
		request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
		request.functionName          = functionName;
		request.stringListArg         = {QStringLiteral("delete_trigger"), QStringLiteral("active trigger")};
		request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
		LuaBatchDispatchResult result;
		dispatchWorkerAndWait(executor, request, result);
		int resumeCount = 0;
		QVERIFY(completeWorkerSuspensions(executor, engine, runtime, result, resumeCount));
		QCOMPARE(resumeCount, 1);
		executeDeferredMutations(result);

		WorldRuntime::LineEntry entry;
		QVERIFY(!runtime.luaContextLineEntryByAbsoluteNumber(deletedLineNumber, entry));
		runtime.endIncomingLineLuaContext();

		LuaBatchDispatchRequest status;
		status.engines      = {engine};
		status.kind         = LuaBatchDispatchKind::StringInOut;
		status.functionName = QStringLiteral("accepted_delete_status");
		status.stringArg    = QStringLiteral("ignored");
		LuaBatchDispatchResult statusResult;
		dispatchWorkerAndWait(executor, status, statusResult);
		QCOMPARE(statusResult.stringResult, expected);
	};

	dispatchDeletion(QStringLiteral("accepted_delete_tail"), QStringLiteral("4|history 1"));
	dispatchDeletion(QStringLiteral("accepted_delete_all"), QStringLiteral("0|nil"));
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerDeletedAnchoredOutputDoesNotAdvanceInsertionCursor()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function delete_anchored_output_then_page(name, line, wildcards)
  Note("first")
  Note("deleted")
  DeleteLines(1)
  GetLineInfo(1, 1)
  Note("after page")
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines                            = {engine};
	request.kind                               = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName                       = QStringLiteral("delete_anchored_output_then_page");
	request.stringListArg                      = {QStringLiteral("anchor_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg              = runtime.luaCallbackSnapshotForBridgedCall();
	request.hasCallbackOutputAnchor            = true;
	request.callbackOutputAnchorBufferIndex    = 1;
	request.callbackOutputAnchorAbsoluteNumber = runtime.lines().constFirst().lineNumber;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(result.suspended);
	QVERIFY(result.hasPendingModalStringRequest);
	executeDeferredMutations(result);
	QCOMPARE(logicalOutputLinesFromEntries(runtime.lines()),
	         QStringList({QStringLiteral("anchor"), QStringLiteral("first")}));

	QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
	result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
	runtime.addLine(QStringLiteral("later output"), WorldRuntime::LineOutput);

	LuaBatchDispatchRequest resume;
	resume.engines       = {engine};
	resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	resume.modalResumeId = result.modalResumeId;
	dispatchWorkerAndWait(executor, resume, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);
	QCOMPARE(logicalOutputLinesFromEntries(runtime.lines()),
	         QStringList({QStringLiteral("anchor"), QStringLiteral("first"), QStringLiteral("after page"),
	                      QStringLiteral("later output")}));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerEmptyTellDoesNotShiftAnchoredOutput()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput | WorldRuntime::LineHidden);
	runtime.addLine(QStringLiteral("after"), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function empty_tell_anchor_cb(name, line, wildcards)
  Tell("")
  Note("inserted")
end
)lua"),
	                       &runtime);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                            = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration           = generation;
	lineSnapshot->lineBufferCount                = count;
	auto snapshot                                = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot              = true;
	snapshot->lineBufferCount                    = count;
	snapshot->lineBufferSnapshot                 = lineSnapshot;
	snapshot->hasCallbackOutputAnchor            = true;
	snapshot->callbackOutputAnchorBufferIndex    = 1;
	snapshot->callbackOutputAnchorAbsoluteNumber = 1;

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("empty_tell_anchor_cb");
	request.stringListArg         = {QStringLiteral("empty_tell_trigger"), QStringLiteral("anchor")};
	request.stringListArg2        = {QStringLiteral("anchor")};
	request.styleRunsArg          = QSharedPointer<QVector<LuaStyleRun>>::create();
	request.miniWindowSnapshotArg = snapshot;
	request.triggerOutputReplacesMatchedLine = true;
	request.triggerMatchedLineBufferIndex    = 1;
	request.triggerMatchedLineAbsoluteNumber = 1;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);
	QCOMPARE(logicalOutputLinesFromEntries(runtime.lines()),
	         QStringList({QStringLiteral("inserted"), QStringLiteral("after")}));
	QCOMPARE(runtime.lines().size(), 2);
	QVERIFY((runtime.lines().first().flags & WorldRuntime::LineHidden) == 0);

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerDeferredOutputDeletionReconcilesPresentation()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 3; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
	runtime.setSessionStateOutputBufferSealed(true);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
deferred_delete_result = ""
function deferred_delete_cb(name, line, wildcards)
  DeleteLines(1)
  local after_delete_lines = GetLinesInBufferCount()
  DeleteOutput()
  deferred_delete_result = string.format("%.0f|%.0f", after_delete_lines, GetInfo(224))
end
function deferred_delete_status(value)
  return deferred_delete_result
end
)lua"),
	                       &runtime);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                    = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration   = generation;
	lineSnapshot->lineBufferCount        = count;
	auto snapshot                        = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot      = true;
	snapshot->lineBufferCount            = count;
	snapshot->lineBufferSnapshot         = lineSnapshot;
	snapshot->hasRuntimeCountersSnapshot = true;
	snapshot->runtimeCounterValues.insert(QStringLiteral("outputLineCount"), count);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("deferred_delete_cb");
	request.stringListArg         = {QStringLiteral("delete_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 4);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 2);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 3);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("deferred_delete_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("3|3"));

	runtime.setSessionStateOutputBufferSealed(false);
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerOutputAfterDeletionDoesNotUseDeletedAnchor()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 3; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
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
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function delete_tail_then_output(name, line, wildcards)
  DeleteLines(1)
  Note("after tail delete")
end
function delete_all_then_output(name, line, wildcards)
  DeleteOutput()
  Note("after full delete")
end
function all_anchored_output_shapes(name, line, wildcards)
  ColourNote("red", "black", "colour note")
  ColourTell("yellow", "blue", "colour tell")
  AnsiNote(ANSI(31) .. "ansi first\n" .. ANSI(32) .. "ansi second\n")
  NoteHr()
end
function replace_anchor_with_ansi(name, line, wildcards)
  AnsiNote(ANSI(31) .. "ansi replacement\n")
end
function replace_anchor_with_multiline_ansi(name, line, wildcards)
  AnsiNote(ANSI(31) .. "first\n" .. ANSI(32) .. "second")
  Note("third")
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.stringListArg         = {QStringLiteral("delete_alias"), QStringLiteral("ignored")};
	request.functionName          = QStringLiteral("delete_tail_then_output");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);
	QCOMPARE(logicalOutputLinesFromEntries(runtime.lines()),
	         QStringList({QStringLiteral("line 1"), QStringLiteral("line 2")}));
	QCOMPARE(outputTexts, QStringList({QStringLiteral("after tail delete")}));

	request.functionName          = QStringLiteral("delete_all_then_output");
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);
	QVERIFY(runtime.lines().isEmpty());
	QCOMPARE(outputTexts,
	         QStringList({QStringLiteral("after tail delete"), QStringLiteral("after full delete")}));

	outputTexts.clear();
	outputSpans.clear();
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);
	request.functionName                       = QStringLiteral("all_anchored_output_shapes");
	request.miniWindowSnapshotArg              = runtime.luaCallbackSnapshotForBridgedCall();
	request.hasCallbackOutputAnchor            = true;
	request.callbackOutputAnchorBufferIndex    = 1;
	request.callbackOutputAnchorAbsoluteNumber = runtime.lines().constLast().lineNumber;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);
	QCOMPARE(
	    logicalOutputLinesFromEntries(runtime.lines()),
	    QStringList({QStringLiteral("anchor"), QStringLiteral("colour note"),
	                 QStringLiteral("colour tellansi first"), QStringLiteral("ansi second"), QString()}));
	QCOMPARE(runtime.lines().size(), 6);
	QVERIFY(runtime.lines().at(1).spans.constFirst().fore == QColor(Qt::red));
	QVERIFY(runtime.lines().at(1).spans.constFirst().back == QColor(Qt::black));
	QVERIFY(runtime.lines().at(2).spans.constFirst().fore == QColor(Qt::yellow));
	QVERIFY(runtime.lines().at(2).spans.constFirst().back == QColor(Qt::blue));
	QVERIFY(runtime.lines().at(3).spans.constFirst().fore == QColor(128, 0, 0));
	QVERIFY(runtime.lines().at(4).spans.constFirst().fore == QColor(0, 128, 0));
	QVERIFY((runtime.lines().constLast().flags & WorldRuntime::LineHorizontalRule) != 0);

	runtime.deleteOutput();
	runtime.addLine(QStringLiteral("vanishing anchor"), WorldRuntime::LineOutput);
	request.miniWindowSnapshotArg              = runtime.luaCallbackSnapshotForBridgedCall();
	request.callbackOutputAnchorBufferIndex    = 1;
	request.callbackOutputAnchorAbsoluteNumber = runtime.lines().constLast().lineNumber;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	runtime.deleteOutput();
	outputTexts.clear();
	outputSpans.clear();
	executeDeferredMutations(result);
	QCOMPARE(outputTexts, QStringList({QStringLiteral("colour note"), QStringLiteral("colour tell"),
	                                   QStringLiteral("ansi first"), QStringLiteral("ansi second")}));
	QCOMPARE(outputSpans.size(), 4);
	QVERIFY(outputSpans.at(0).constFirst().fore == QColor(Qt::red));
	QVERIFY(outputSpans.at(0).constFirst().back == QColor(Qt::black));
	QVERIFY(outputSpans.at(1).constFirst().fore == QColor(Qt::yellow));
	QVERIFY(outputSpans.at(1).constFirst().back == QColor(Qt::blue));
	QVERIFY(outputSpans.at(2).constFirst().fore == QColor(128, 0, 0));
	QVERIFY(outputSpans.at(3).constFirst().fore == QColor(0, 128, 0));
	QCOMPARE(runtime.lines().size(), 1);
	QVERIFY((runtime.lines().constFirst().flags & WorldRuntime::LineHorizontalRule) != 0);

	WorldRuntime::LineEntry hiddenAnchor;
	hiddenAnchor.text       = QStringLiteral("hidden trigger");
	hiddenAnchor.flags      = WorldRuntime::LineOutput | WorldRuntime::LineHidden;
	hiddenAnchor.hardReturn = true;
	hiddenAnchor.lineNumber = 9001;
	runtime.replaceOutputLines({hiddenAnchor});
	request.functionName      = QStringLiteral("replace_anchor_with_ansi");
	request.stringListArg     = {QStringLiteral("trigger"), hiddenAnchor.text};
	request.stringListArg2    = {hiddenAnchor.text};
	auto replacementStyleRuns = QSharedPointer<QVector<LuaStyleRun>>::create();
	replacementStyleRuns->push_back({hiddenAnchor.text, 0xFFFFFF, 0, 0});
	request.styleRunsArg                       = replacementStyleRuns;
	request.miniWindowSnapshotArg              = runtime.luaCallbackSnapshotForBridgedCall();
	request.hasCallbackOutputAnchor            = false;
	request.callbackOutputAnchorBufferIndex    = 0;
	request.callbackOutputAnchorAbsoluteNumber = 0;
	request.triggerMatchedLineBufferIndex      = 1;
	request.triggerMatchedLineAbsoluteNumber   = hiddenAnchor.lineNumber;
	request.triggerOutputReplacesMatchedLine   = true;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);
	QCOMPARE(runtime.lines().size(), 1);
	QCOMPARE(runtime.lines().constFirst().text, QStringLiteral("ansi replacement"));
	QVERIFY((runtime.lines().constFirst().flags & WorldRuntime::LineHidden) == 0);
	QVERIFY(runtime.lines().constFirst().spans.constFirst().fore == QColor(128, 0, 0));

	hiddenAnchor.text       = QStringLiteral("second hidden trigger");
	hiddenAnchor.lineNumber = 9002;
	runtime.replaceOutputLines({hiddenAnchor});
	request.functionName   = QStringLiteral("replace_anchor_with_multiline_ansi");
	request.stringListArg  = {QStringLiteral("trigger"), hiddenAnchor.text};
	request.stringListArg2 = {hiddenAnchor.text};
	replacementStyleRuns->clear();
	replacementStyleRuns->push_back({hiddenAnchor.text, 0xFFFFFF, 0, 0});
	request.miniWindowSnapshotArg            = runtime.luaCallbackSnapshotForBridgedCall();
	request.triggerMatchedLineAbsoluteNumber = hiddenAnchor.lineNumber;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);
	QCOMPARE(logicalOutputLinesFromEntries(runtime.lines()),
	         QStringList({QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third")}));
	QCOMPARE(runtime.lines().size(), 3);
	QCOMPARE(runtime.lines().at(0).text, QStringLiteral("first"));
	QCOMPARE(runtime.lines().at(1).text, QStringLiteral("second"));
	QCOMPARE(runtime.lines().at(2).text, QStringLiteral("third"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerAnsiNoteTerminatesAndPreservesUtf8()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function ansi_note_semantics(name, line, wildcards)
  AnsiNote(ANSI(31) .. "café")
  AnsiNote(ANSI(32))
  AnsiNote("")
  AnsiNote(ANSI(31) .. "dark" .. ANSI(1) .. "bright" .. ANSI(22) .. "dark2" ..
           ANSI(7) .. "inverse" .. ANSI(27) .. "plain" .. ANSI(0) .. "reset")
  AnsiNote("\27[38:2::10:20:30;48:2::40:50:60mcolon" ..
           "\27[38:2::128:0:0mtrue\27[1mboldtrue\27[22mplaintrue" ..
           "\27[1;91mbright\27[22mstillbright")
  AnsiNote(string.char(27) .. "]8;;https://example.org" .. string.char(7) ..
           "linked" .. string.char(27) .. "]8;;" .. string.char(7) .. " plain")
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines                            = {engine};
	request.kind                               = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName                       = QStringLiteral("ansi_note_semantics");
	request.stringListArg                      = {QStringLiteral("ansi_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg              = runtime.luaCallbackSnapshotForBridgedCall();
	request.hasCallbackOutputAnchor            = true;
	request.callbackOutputAnchorBufferIndex    = 1;
	request.callbackOutputAnchorAbsoluteNumber = runtime.lines().constFirst().lineNumber;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);

	QCOMPARE(runtime.lines().size(), 7);
	QCOMPARE(runtime.lines().at(1).text, QStringLiteral("caf\u00e9"));
	QVERIFY(runtime.lines().at(1).hardReturn);
	QVERIFY(!runtime.lines().at(1).spans.isEmpty());
	QCOMPARE(runtime.lines().at(1).spans.constFirst().fore, QColor(128, 0, 0));
	QVERIFY(runtime.lines().at(2).text.isEmpty());
	QVERIFY(runtime.lines().at(2).hardReturn);
	QVERIFY(runtime.lines().at(3).text.isEmpty());
	QVERIFY(runtime.lines().at(3).hardReturn);
	const WorldRuntime::LineEntry &styledLine = runtime.lines().at(4);
	QCOMPARE(styledLine.text, QStringLiteral("darkbrightdark2inverseplainreset"));
	QCOMPARE(styledLine.spans.size(), 6);
	QCOMPARE(styledLine.spans.at(0).fore, QColor(128, 0, 0));
	QVERIFY(!styledLine.spans.at(0).bold);
	QCOMPARE(styledLine.spans.at(1).fore, QColor(255, 0, 0));
	QVERIFY(styledLine.spans.at(1).bold);
	QCOMPARE(styledLine.spans.at(2).fore, QColor(128, 0, 0));
	QVERIFY(!styledLine.spans.at(2).bold);
	QCOMPARE(styledLine.spans.at(3).fore, QColor(128, 0, 0));
	QCOMPARE(styledLine.spans.at(3).back, QColor(0, 0, 0));
	QVERIFY(styledLine.spans.at(3).inverse);
	QCOMPARE(styledLine.spans.at(4).fore, QColor(128, 0, 0));
	QCOMPARE(styledLine.spans.at(4).back, QColor(0, 0, 0));
	QVERIFY(!styledLine.spans.at(4).inverse);
	QCOMPARE(styledLine.spans.at(5).fore, QColor(192, 192, 192));
	QCOMPARE(styledLine.spans.at(5).back, QColor(0, 0, 0));

	const WorldRuntime::LineEntry &extendedLine = runtime.lines().at(5);
	QCOMPARE(extendedLine.text, QStringLiteral("colontrueboldtrueplaintruebrightstillbright"));
	QCOMPARE(extendedLine.spans.size(), 6);
	QCOMPARE(extendedLine.spans.at(0).fore, QColor(10, 20, 30));
	QCOMPARE(extendedLine.spans.at(0).back, QColor(40, 50, 60));
	QCOMPARE(extendedLine.spans.at(1).fore, QColor(128, 0, 0));
	QVERIFY(!extendedLine.spans.at(1).bold);
	QCOMPARE(extendedLine.spans.at(2).fore, QColor(128, 0, 0));
	QVERIFY(extendedLine.spans.at(2).bold);
	QCOMPARE(extendedLine.spans.at(3).fore, QColor(128, 0, 0));
	QVERIFY(!extendedLine.spans.at(3).bold);
	QCOMPARE(extendedLine.spans.at(4).fore, QColor(255, 0, 0));
	QVERIFY(extendedLine.spans.at(4).bold);
	QCOMPARE(extendedLine.spans.at(5).fore, QColor(255, 0, 0));
	QVERIFY(!extendedLine.spans.at(5).bold);

	const WorldRuntime::LineEntry &linkedLine = runtime.lines().at(6);
	QCOMPARE(linkedLine.text, QStringLiteral("linked plain"));
	QCOMPARE(linkedLine.spans.size(), 2);
	QCOMPARE(linkedLine.spans.at(0).actionType, static_cast<int>(WorldRuntime::ActionHyperlink));
	QCOMPARE(linkedLine.spans.at(0).action, QStringLiteral("https://example.org"));
	QCOMPARE(linkedLine.spans.at(1).actionType, static_cast<int>(WorldRuntime::ActionNone));
	QVERIFY(linkedLine.spans.at(1).action.isEmpty());

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerNoteHrTerminatesOpenOutputLine()
{
	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("log_notes"), QStringLiteral("1"));
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function tell_then_rule(name, line, wildcards)
  ColourTell("white", "black", "open line")
  NoteHr()
  rule_cache_state = tostring(GetLineInfo(2, 3))
end
function tell_then_rule_status(value)
  return rule_cache_state
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines                            = {engine};
	request.kind                               = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName                       = QStringLiteral("tell_then_rule");
	request.stringListArg                      = {QStringLiteral("rule_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg              = runtime.luaCallbackSnapshotForBridgedCall();
	request.hasCallbackOutputAnchor            = true;
	request.callbackOutputAnchorBufferIndex    = 1;
	request.callbackOutputAnchorAbsoluteNumber = runtime.lines().constFirst().lineNumber;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 2);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);

	QCOMPARE(runtime.lines().size(), 3);
	QCOMPARE(runtime.lines().at(1).text, QStringLiteral("open line"));
	QVERIFY(runtime.lines().at(1).hardReturn);
	QVERIFY(runtime.lines().at(2).text.isEmpty());
	QVERIFY(runtime.lines().at(2).hardReturn);
	QVERIFY((runtime.lines().at(2).flags & WorldRuntime::LineHorizontalRule) != 0);
	QVERIFY((runtime.lines().at(2).flags & WorldRuntime::LineLog) == 0);
	QCOMPARE(logicalOutputLinesFromEntries(runtime.lines()),
	         QStringList({QStringLiteral("anchor"), QStringLiteral("open line"), QString()}));

	LuaBatchDispatchRequest statusRequest;
	statusRequest.engines      = {engine};
	statusRequest.kind         = LuaBatchDispatchKind::StringInOut;
	statusRequest.functionName = QStringLiteral("tell_then_rule_status");
	statusRequest.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, statusRequest, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("true"));

	WorldRuntime viewRuntime;
	viewRuntime.setWorldAttribute(QStringLiteral("log_notes"), QStringLiteral("1"));
	WorldView view;
	view.setRuntime(&viewRuntime);
	view.appendNoteText(QStringLiteral("ordinary open line"), false);
	view.appendHorizontalRule();
	QCOMPARE(viewRuntime.lines().size(), 2);
	QVERIFY(viewRuntime.lines().at(0).hardReturn);
	QVERIFY((viewRuntime.lines().at(1).flags & WorldRuntime::LineHorizontalRule) != 0);
	QVERIFY((viewRuntime.lines().at(1).flags & WorldRuntime::LineLog) == 0);

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerScreendrawReceivesCompletedPresentedLines()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);

	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	auto              screenEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, screenEngine, QStringLiteral(R"lua(
screen_calls = {}
function OnPluginScreendraw(draw_type, log, text)
  table.insert(screen_calls, string.format("%.0f,%.0f,%s", draw_type, log, text))
  if text == "open line one" then Note("nested guard output") end
end
function screen_status(command)
  local result = table.concat(screen_calls, "|")
  if command == "reset" then screen_calls = {} end
  return result
end
)lua"),
	                       &runtime, QStringLiteral("screen.plugin"));
	WorldRuntime::Plugin screenPlugin;
	screenPlugin.attributes.insert(QStringLiteral("id"), QStringLiteral("screen.plugin"));
	screenPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("Screen plugin"));
	screenPlugin.attributes.insert(QStringLiteral("language"), QStringLiteral("lua"));
	screenPlugin.lua = screenEngine;
	runtime.pluginsMutable().push_back(screenPlugin);

	auto outputEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, outputEngine, QStringLiteral(R"lua(
function completed_lines(name, line, wildcards)
  ColourTell("white", "black", "open ")
  ColourTell("white", "black", "line ")
  AnsiNote(ANSI(31) .. "one\n" .. ANSI(32) .. "two")
end
function rule_closes_tell(name, line, wildcards)
  ColourTell("white", "black", "hr open")
  NoteHr()
end
function ansi_fallback(name, line, wildcards)
  AnsiNote(ANSI(31) .. "fallback")
end
)lua"),
	                       &runtime, QStringLiteral("output.plugin"));

	auto dispatchOutput = [&](const QString &functionName, const bool anchored)
	{
		LuaBatchDispatchRequest request;
		request.engines               = {outputEngine};
		request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
		request.functionName          = functionName;
		request.stringListArg         = {QStringLiteral("output_alias"), QStringLiteral("ignored")};
		request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
		if (anchored)
		{
			request.hasCallbackOutputAnchor            = true;
			request.callbackOutputAnchorBufferIndex    = safeQSizeToInt(runtime.lines().size());
			request.callbackOutputAnchorAbsoluteNumber = runtime.lines().constLast().lineNumber;
		}
		LuaBatchDispatchResult result;
		dispatchWorkerAndWait(executor, request, result);
		QVERIFY(!result.suspended);
		executeDeferredMutations(result);
	};
	auto screenStatus = [&](const QString &command)
	{
		LuaBatchDispatchRequest request;
		request.engines      = {screenEngine};
		request.kind         = LuaBatchDispatchKind::StringInOut;
		request.functionName = QStringLiteral("screen_status");
		request.stringArg    = command;
		LuaBatchDispatchResult result;
		dispatchWorkerAndWait(executor, request, result);
		return result.stringResult;
	};
	auto waitForScreenStatus = [&](const QString &expected)
	{
		QString       actual;
		QElapsedTimer timer;
		timer.start();
		do
		{
			actual = screenStatus(QString());
			if (actual == expected)
				break;
			QTest::qWait(10);
		} while (timer.elapsed() < 2000);
		QCOMPARE(actual, expected);
	};

	dispatchOutput(QStringLiteral("completed_lines"), true);
	waitForScreenStatus(QStringLiteral("1,0,open line one|1,0,two"));
	QCOMPARE(screenStatus(QString()), QStringLiteral("1,0,open line one|1,0,two"));

	runtime.pluginsMutable().clear();
	teardownWorkerEngine(executor, outputEngine);
	teardownWorkerEngine(executor, screenEngine);

	WorldRuntime fallbackRuntime;
	fallbackRuntime.addLine(QStringLiteral("hr open"), WorldRuntime::LineNote, false);
	auto fallbackScreenEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, fallbackScreenEngine, QStringLiteral(R"lua(
fallback_screen_lines = {}
function OnPluginScreendraw(draw_type, log, text)
  table.insert(fallback_screen_lines, string.format("%.0f,%.0f,%s", draw_type, log, text))
end
function fallback_screen_status(command)
  return table.concat(fallback_screen_lines, "|")
end
)lua"),
	                       &fallbackRuntime, QStringLiteral("fallback.screen.plugin"));
	WorldRuntime::Plugin fallbackScreenPlugin;
	fallbackScreenPlugin.attributes.insert(QStringLiteral("id"), QStringLiteral("fallback.screen.plugin"));
	fallbackScreenPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("Fallback screen plugin"));
	fallbackScreenPlugin.attributes.insert(QStringLiteral("language"), QStringLiteral("lua"));
	fallbackScreenPlugin.lua = fallbackScreenEngine;
	fallbackRuntime.pluginsMutable().push_back(fallbackScreenPlugin);

	auto fallbackOutputEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, fallbackOutputEngine, QStringLiteral(R"lua(
function OnPluginSent(text)
  NoteHr()
  DeleteOutput()
  AnsiNote(ANSI(31) .. "fallback")
end
)lua"),
	                       &fallbackRuntime, QStringLiteral("fallback.output.plugin"));
	WorldRuntime::Plugin fallbackOutputPlugin;
	fallbackOutputPlugin.attributes.insert(QStringLiteral("id"), QStringLiteral("fallback.output.plugin"));
	fallbackOutputPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("Fallback output plugin"));
	fallbackOutputPlugin.attributes.insert(QStringLiteral("language"), QStringLiteral("lua"));
	fallbackOutputPlugin.lua = fallbackOutputEngine;
	fallbackRuntime.pluginsMutable().push_back(fallbackOutputPlugin);
	fallbackRuntime.firePluginSent(QStringLiteral("go"));

	LuaBatchDispatchRequest fallbackStatus;
	fallbackStatus.engines      = {fallbackScreenEngine};
	fallbackStatus.kind         = LuaBatchDispatchKind::StringInOut;
	fallbackStatus.functionName = QStringLiteral("fallback_screen_status");
	fallbackStatus.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult fallbackStatusResult;
	QElapsedTimer          fallbackTimer;
	fallbackTimer.start();
	do
	{
		QTest::qWait(10);
		dispatchWorkerAndWait(executor, fallbackStatus, fallbackStatusResult);
	} while (fallbackStatusResult.stringResult != QStringLiteral("1,0,hr open|1,0,fallback") &&
	         fallbackTimer.elapsed() < 2000);
	QCOMPARE(fallbackStatusResult.stringResult, QStringLiteral("1,0,hr open|1,0,fallback"));

	fallbackRuntime.pluginsMutable().clear();
	teardownWorkerEngine(executor, fallbackOutputEngine);
	teardownWorkerEngine(executor, fallbackScreenEngine);
}

void tst_LuaCallbackEngine::workerDrawOutputWindowOutputDoesNotQueueRecursiveCallback()
{
	QTemporaryDir tempDir;
	QVERIFY(tempDir.isValid());
	const QString pluginsDir = QDir(tempDir.path()).filePath(QStringLiteral("worlds/plugins"));
	QVERIFY(QDir().mkpath(pluginsDir));
	QFile pluginFile(QDir(pluginsDir).filePath(QStringLiteral("draw_output.xml")));
	QVERIFY(pluginFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
	const QByteArray pluginXml = QByteArrayLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="DrawOutputRegression"
    author="QMud Test"
    id="fedcba9876543210fedcba98"
    language="lua"
    enabled="y"
    save_state="n">
    <script><![CDATA[
function OnPluginDrawOutputWindow(first_line, offset)
  local calls = tonumber(GetVariable("draw_output_calls") or "0") + 1
  SetVariable("draw_output_calls", tostring(calls))
  SetVariable("draw_output_last_position", string.format("%.0f,%.0f", first_line, offset))
  SetVariable("draw_output_last_redraw_count", string.format("%.0f", GetInfo(295)))
  SetVariable("draw_output_last_action_source", string.format("%.0f", GetInfo(239)))
  local mode = GetVariable("draw_output_mode")
  if mode == "output" and calls == 1 then
    Note("draw callback output")
  elseif mode == "delay" and calls == 1 then
    local finish = os.clock() + 0.25
    while os.clock() < finish do end
  elseif mode == "priority" then
    draw_priority_order = (draw_priority_order or "") .. "draw,"
    SetVariable("draw_priority_order", draw_priority_order)
  elseif mode == "page" and calls == 1 then
    page_resume_order = "A-before,"
    SetVariable("page_resume_line", GetLineInfo(1, 1) or "<missing>")
    page_resume_order = page_resume_order .. "A-after,"
    SetVariable("page_resume_order", page_resume_order)
  end
end
function qcb_draw_priority(name, line, wildcards)
  draw_priority_order = (draw_priority_order or "") .. "critical,"
  SetVariable("draw_priority_order", draw_priority_order)
end
function qcb_page_priority(name, line, wildcards)
  page_resume_order = (page_resume_order or "") .. "B,"
  SetVariable("page_resume_order", page_resume_order)
end
]]></script>
  </plugin>
</muclient>
)xml");
	QCOMPARE(pluginFile.write(pluginXml), pluginXml.size());
	pluginFile.close();

	QFile recipientPluginFile(QDir(pluginsDir).filePath(QStringLiteral("draw_output_recipient.xml")));
	QVERIFY(recipientPluginFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
	const QByteArray recipientPluginXml = QByteArrayLiteral(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<muclient>
  <plugin
    name="DrawOutputRecipientRegression"
    author="QMud Test"
    id="0123456789abcdef01234567"
    language="lua"
    enabled="y"
    save_state="n">
    <script><![CDATA[
function OnPluginEnable()
  local order = (GetVariable("draw_output_order") or "") .. "enable,"
  SetVariable("draw_output_order", order)
end
function OnPluginDrawOutputWindow(first_line, offset)
  local calls = tonumber(GetVariable("draw_output_calls") or "0") + 1
  SetVariable("draw_output_calls", tostring(calls))
  SetVariable("draw_output_last_position", string.format("%.0f,%.0f", first_line, offset))
  local order = (GetVariable("draw_output_order") or "") .. "draw,"
  SetVariable("draw_output_order", order)
end
]]></script>
  </plugin>
</muclient>
)xml");
	QCOMPARE(recipientPluginFile.write(recipientPluginXml), recipientPluginXml.size());
	recipientPluginFile.close();

	WorldRuntime runtime;
	runtime.setStartupDirectory(tempDir.path());
	runtime.setPluginsDirectory(QStringLiteral("worlds/plugins"));
	WorldView view;
	view.setRuntime(&runtime);
	QObject::connect(&runtime, &WorldRuntime::outputRequested, &view,
	                 [&view](const QString &text, const bool newLine, const bool note)
	                 {
		                 if (note)
			                 view.appendNoteText(text, newLine);
		                 else
			                 view.appendOutputText(text, newLine);
	                 });
	QObject::connect(&runtime, &WorldRuntime::outputStyledRequested, &view,
	                 [&view](const QString &text, const QVector<WorldRuntime::StyleSpan> &spans,
	                         const bool newLine, const bool note)
	                 {
		                 if (note)
			                 view.appendNoteTextStyled(text, spans, newLine);
		                 else
			                 view.appendOutputTextStyled(text, spans, newLine);
	                 });
	view.resize(500, 240);
	view.show();
	for (int line = 0; line < 120; ++line)
		runtime.outputText(QStringLiteral("baseline %1").arg(line), false, true);
	QTRY_VERIFY_WITH_TIMEOUT(view.outputScrollPosition() > 0, 2000);

	QString loadError;
	QVERIFY2(runtime.loadPluginFile(QStringLiteral("draw_output.xml"), &loadError), qPrintable(loadError));
	QVERIFY2(runtime.loadPluginFile(QStringLiteral("draw_output_recipient.xml"), &loadError),
	         qPrintable(loadError));
	QTRY_COMPARE_WITH_TIMEOUT(runtime.plugins().size(), 2, 5000);
	QTRY_VERIFY_WITH_TIMEOUT(std::ranges::none_of(runtime.plugins(), [](const WorldRuntime::Plugin &plugin)
	                                              { return plugin.installPending; }),
	                         5000);
	const QString kDrawPluginId      = QStringLiteral("fedcba9876543210fedcba98");
	const QString kRecipientPluginId = QStringLiteral("0123456789abcdef01234567");
	auto          drawPluginIt =
	    std::ranges::find_if(runtime.pluginsMutable(), [&kDrawPluginId](const WorldRuntime::Plugin &plugin)
	                         { return plugin.attributes.value(QStringLiteral("id")) == kDrawPluginId; });
	QVERIFY(drawPluginIt != runtime.pluginsMutable().end());
	const QSharedPointer<LuaCallbackEngine> drawPluginEngine = drawPluginIt->lua;
	QVERIFY(drawPluginEngine);
	QVERIFY(runtime.enablePlugin(kRecipientPluginId, false));

	const auto drawOutputVariable = [&runtime](const QString &pluginId, const QString &name)
	{
		QString value;
		static_cast<void>(runtime.findPluginVariable(pluginId, name, value));
		return value;
	};
	const auto drawOutputCallCount = [&]
	{ return drawOutputVariable(kDrawPluginId, QStringLiteral("draw_output_calls")); };

	// Drain presentation notifications queued while the view and plugin were initialized, then start
	// this check from a known callback count. Only output produced by the explicit callback below matters.
	QTest::qWait(1000);
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_mode"),
	                               QStringLiteral("output"));
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_calls"), QStringLiteral("0"));
	const qsizetype baselineLineCount = runtime.lines().size();
	runtime.notifyDrawOutputWindow(1, 0);
	QTRY_COMPARE_WITH_TIMEOUT(drawOutputCallCount(), QStringLiteral("1"), 2000);
	QTest::qWait(250);
	QCOMPARE(drawOutputCallCount(), QStringLiteral("1"));
	QCOMPARE(runtime.lines().size(), baselineLineCount + 1);
	QCOMPARE(runtime.lines().constLast().text, QStringLiteral("draw callback output"));

	WorldView observer;
	observer.resize(500, 240);
	observer.setPassiveBufferView(true);
	observer.setRuntimeObserver(&runtime);
	observer.show();
	QTRY_VERIFY_WITH_TIMEOUT(observer.outputScrollPosition() > 0, 2000);
	QTest::qWait(250);

	// A passive observer may move its own viewport while a draw callback is active, but that movement
	// must not enqueue another runtime callback.
	observer.scrollOutputToEnd();
	QTest::qWait(250);
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_mode"),
	                               QStringLiteral("delay"));
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_calls"), QStringLiteral("0"));
	bool delayedDrawCompleted = false;
	runtime.notifyDrawOutputWindow(1, 0, [&delayedDrawCompleted] { delayedDrawCompleted = true; });
	QTRY_VERIFY_WITH_TIMEOUT(runtime.drawOutputWindowCallbackActive(), 1000);
	QVERIFY(!delayedDrawCompleted);
	const int endScrollPosition = observer.outputScrollPosition();
	QVERIFY(endScrollPosition > 0);
	observer.scrollOutputToStart();
	QCOMPARE(observer.outputScrollPosition(), 0);
	QTRY_COMPARE_WITH_TIMEOUT(drawOutputCallCount(), QStringLiteral("1"), 3000);
	QTRY_VERIFY_WITH_TIMEOUT(delayedDrawCompleted, 1000);
	QTest::qWait(250);
	QCOMPARE(drawOutputCallCount(), QStringLiteral("1"));

	// Draw callbacks retain their callback-lane position at notification time. In the four-event
	// sequence draw A, critical callback B, draw C, critical callback D, neither critical callback
	// may overtake the preceding draw and draw C may not be deferred behind callback D.
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_mode"),
	                               QStringLiteral("priority"));
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_priority_order"), QString());
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_calls"), QStringLiteral("0"));
	runtime.notifyDrawOutputWindow(2, 20);
	bool priorityCallbackCompleted = false;
	runtime.dispatchLuaStringsAndWildcardsAsync(
	    drawPluginEngine, QStringLiteral("qcb_draw_priority"),
	    {QStringLiteral("priority"), QStringLiteral("ignored")}, {}, {}, nullptr, -1, false, 1, 1,
	    [&priorityCallbackCompleted](const LuaBatchDispatchResult & /*unused*/)
	    { priorityCallbackCompleted = true; });
	runtime.notifyDrawOutputWindow(3, 30);
	bool trailingPriorityCallbackCompleted = false;
	runtime.dispatchLuaStringsAndWildcardsAsync(
	    drawPluginEngine, QStringLiteral("qcb_draw_priority"),
	    {QStringLiteral("priority"), QStringLiteral("ignored")}, {}, {}, nullptr, -1, false, 1, 1,
	    [&trailingPriorityCallbackCompleted](const LuaBatchDispatchResult & /*unused*/)
	    { trailingPriorityCallbackCompleted = true; });
	QTRY_VERIFY_WITH_TIMEOUT(priorityCallbackCompleted, 2000);
	QTRY_VERIFY_WITH_TIMEOUT(trailingPriorityCallbackCompleted, 2000);
	QTRY_COMPARE_WITH_TIMEOUT(drawOutputVariable(kDrawPluginId, QStringLiteral("draw_priority_order")),
	                          QStringLiteral("draw,critical,draw,critical,"), 2000);
	QCOMPARE(drawOutputCallCount(), QStringLiteral("2"));
	QCOMPARE(drawOutputVariable(kDrawPluginId, QStringLiteral("draw_output_last_position")),
	         QStringLiteral("3,30"));

	// Fetching an uncached output page is an implementation detail inside the first callback. Its
	// immediate continuation must retain that callback's lane position instead of allowing a later
	// input-critical callback to split the callback into two independently ordered halves.
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_mode"), QStringLiteral("page"));
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_calls"), QStringLiteral("0"));
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("page_resume_order"), QString());
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("page_resume_line"), QString());
	runtime.notifyDrawOutputWindow(4, 40);
	bool pagePriorityCallbackCompleted = false;
	runtime.dispatchLuaStringsAndWildcardsAsync(
	    drawPluginEngine, QStringLiteral("qcb_page_priority"),
	    {QStringLiteral("page_priority"), QStringLiteral("ignored")}, {}, {}, nullptr, -1, false, 1, 1,
	    [&pagePriorityCallbackCompleted](const LuaBatchDispatchResult & /*unused*/)
	    { pagePriorityCallbackCompleted = true; });
	QTRY_VERIFY_WITH_TIMEOUT(pagePriorityCallbackCompleted, 2000);
	QTRY_COMPARE_WITH_TIMEOUT(drawOutputVariable(kDrawPluginId, QStringLiteral("page_resume_order")),
	                          QStringLiteral("A-before,A-after,B,"), 2000);
	QCOMPARE(drawOutputVariable(kDrawPluginId, QStringLiteral("page_resume_line")),
	         QStringLiteral("baseline 0"));

	// A newer queued event replaces the complete event snapshot, not just its numeric arguments.
	// The public redraw counter still advances for every presentation notification even though Lua
	// receives only the final coalesced callback.
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_mode"), QString());
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_calls"), QStringLiteral("0"));
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_last_position"), QString());
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_last_redraw_count"), QString());
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_last_action_source"),
	                               QString());
	const int     redrawCountBeforeCoalescing = runtime.outputWindowRedrawCount();
	constexpr int kCoalescedDrawCount         = 4096;
	int           coalescedCompletionCount    = 0;
	runtime.setCurrentActionSource(WorldRuntime::eWorldAction);
	for (int draw = 0; draw < kCoalescedDrawCount - 1; ++draw)
	{
		runtime.notifyDrawOutputWindow(draw + 2, draw + 20,
		                               [&coalescedCompletionCount] { ++coalescedCompletionCount; });
	}
	runtime.setCurrentActionSource(WorldRuntime::eHotspotCallback);
	runtime.notifyDrawOutputWindow(kCoalescedDrawCount + 1, kCoalescedDrawCount + 19,
	                               [&coalescedCompletionCount] { ++coalescedCompletionCount; });
	runtime.setCurrentActionSource(WorldRuntime::eUnknownActionSource);
	QTRY_COMPARE_WITH_TIMEOUT(drawOutputCallCount(), QStringLiteral("1"), 2000);
	QCOMPARE(drawOutputVariable(kDrawPluginId, QStringLiteral("draw_output_last_position")),
	         QStringLiteral("4097,4115"));
	QCOMPARE(runtime.outputWindowRedrawCount(), redrawCountBeforeCoalescing + kCoalescedDrawCount);
	QCOMPARE(drawOutputVariable(kDrawPluginId, QStringLiteral("draw_output_last_redraw_count")),
	         QString::number(redrawCountBeforeCoalescing + kCoalescedDrawCount));
	QCOMPARE(drawOutputVariable(kDrawPluginId, QStringLiteral("draw_output_last_action_source")),
	         QString::number(WorldRuntime::eHotspotCallback));
	QCOMPARE(coalescedCompletionCount, kCoalescedDrawCount);
	QTest::qWait(250);
	QCOMPARE(drawOutputCallCount(), QStringLiteral("1"));

	// A lifecycle callback queued between draw events is an ordering boundary. The first draw keeps
	// its original recipients; OnPluginEnable runs next; only the later draw includes the newly
	// enabled recipient.
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_calls"), QStringLiteral("0"));
	runtime.setPluginVariableValue(kRecipientPluginId, QStringLiteral("draw_output_calls"),
	                               QStringLiteral("0"));
	runtime.setPluginVariableValue(kRecipientPluginId, QStringLiteral("draw_output_order"), QString());
	runtime.notifyDrawOutputWindow(5, 50);
	QVERIFY(runtime.enablePlugin(kRecipientPluginId, true));
	runtime.notifyDrawOutputWindow(6, 60);
	QTRY_COMPARE_WITH_TIMEOUT(drawOutputCallCount(), QStringLiteral("2"), 2000);
	QTRY_COMPARE_WITH_TIMEOUT(drawOutputVariable(kRecipientPluginId, QStringLiteral("draw_output_calls")),
	                          QStringLiteral("1"), 2000);
	QCOMPARE(drawOutputVariable(kRecipientPluginId, QStringLiteral("draw_output_order")),
	         QStringLiteral("enable,draw,"));
	QCOMPARE(drawOutputVariable(kDrawPluginId, QStringLiteral("draw_output_last_position")),
	         QStringLiteral("6,60"));
	QCOMPARE(drawOutputVariable(kRecipientPluginId, QStringLiteral("draw_output_last_position")),
	         QStringLiteral("6,60"));

	// Demoting a directly bound view while a draw callback is running must release its callback state.
	// Once passive, later viewport changes must not reach runtime callbacks.
	view.scrollOutputToEnd();
	QTRY_VERIFY_WITH_TIMEOUT(view.outputScrollPosition() > 0, 1000);
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_mode"),
	                               QStringLiteral("delay"));
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_calls"), QStringLiteral("0"));
	runtime.notifyDrawOutputWindow(7, 70);
	QTRY_VERIFY_WITH_TIMEOUT(runtime.drawOutputWindowCallbackActive(), 1000);
	view.setPassiveBufferView(true);
	view.setRuntimeObserver(&runtime);
	QTRY_COMPARE_WITH_TIMEOUT(drawOutputCallCount(), QStringLiteral("1"), 3000);
	QTRY_VERIFY_WITH_TIMEOUT(!runtime.drawOutputWindowCallbackActive(), 1000);
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_mode"), QString());
	runtime.setPluginVariableValue(kDrawPluginId, QStringLiteral("draw_output_calls"), QStringLiteral("0"));
	view.scrollOutputToStart();
	QCOMPARE(view.outputScrollPosition(), 0);
	QTest::qWait(250);
	QCOMPARE(drawOutputCallCount(), QStringLiteral("0"));

	observer.setRuntimeObserver(nullptr);
	view.setRuntime(nullptr);
}

void tst_LuaCallbackEngine::workerAnchoredOutputWrapsFromOpenLineColumn()
{
	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("wrap"), QStringLiteral("1"));
	runtime.setWorldAttribute(QStringLiteral("auto_wrap_window_width"), QStringLiteral("0"));
	runtime.setWorldAttribute(QStringLiteral("wrap_column"), QStringLiteral("10"));
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function wrap_open_line(name, line, wildcards)
  ColourTell("white", "black", "1234 ")
  AnsiNote("abcdef")
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines                            = {engine};
	request.kind                               = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName                       = QStringLiteral("wrap_open_line");
	request.stringListArg                      = {QStringLiteral("wrap_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg              = runtime.luaCallbackSnapshotForBridgedCall();
	request.hasCallbackOutputAnchor            = true;
	request.callbackOutputAnchorBufferIndex    = 1;
	request.callbackOutputAnchorAbsoluteNumber = runtime.lines().constFirst().lineNumber;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);

	QCOMPARE(logicalOutputLinesFromEntries(runtime.lines()),
	         QStringList({QStringLiteral("anchor"), QStringLiteral("1234 "), QStringLiteral("abcdef")}));
	QCOMPARE(runtime.lines().size(), 4);
	QCOMPARE(runtime.lines().at(1).text, QStringLiteral("1234 "));
	QVERIFY(!runtime.lines().at(1).hardReturn);
	QVERIFY(runtime.lines().at(2).text.isEmpty());
	QVERIFY(runtime.lines().at(2).hardReturn);
	QCOMPARE(runtime.lines().at(3).text, QStringLiteral("abcdef"));
	QVERIFY(runtime.lines().at(3).hardReturn);

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerAnchoredOutputCommitsBeforeScreendrawMutation()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);

	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	auto              outputEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, outputEngine, QStringLiteral(R"lua(
function anchored_note(name, line, wildcards)
  Note("inserted")
end
)lua"),
	                       &runtime, QStringLiteral("output.plugin"));

	auto screenEngine = QSharedPointer<LuaCallbackEngine>::create();
	initializeWorkerEngine(executor, screenEngine, QStringLiteral(R"lua(
function OnPluginScreendraw(type, log, text)
  DeleteOutput()
end
)lua"),
	                       &runtime, QStringLiteral("screen.plugin"));
	WorldRuntime::Plugin screenPlugin;
	screenPlugin.attributes.insert(QStringLiteral("id"), QStringLiteral("screen.plugin"));
	screenPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("Screen plugin"));
	screenPlugin.attributes.insert(QStringLiteral("language"), QStringLiteral("lua"));
	screenPlugin.lua = screenEngine;
	runtime.pluginsMutable().push_back(screenPlugin);

	LuaBatchDispatchRequest request;
	request.engines                            = {outputEngine};
	request.kind                               = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName                       = QStringLiteral("anchored_note");
	request.stringListArg                      = {QStringLiteral("note_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg              = runtime.luaCallbackSnapshotForBridgedCall();
	request.hasCallbackOutputAnchor            = true;
	request.callbackOutputAnchorBufferIndex    = 1;
	request.callbackOutputAnchorAbsoluteNumber = runtime.lines().constFirst().lineNumber;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);

	QVERIFY(runtime.lines().isEmpty());

	runtime.pluginsMutable().clear();
	teardownWorkerEngine(executor, screenEngine);
	teardownWorkerEngine(executor, outputEngine);
}

void tst_LuaCallbackEngine::workerNestedDispatchRefreshesDirtyLinePresentation()
{
	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("max_output_lines"), QStringLiteral("400"));
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              caller   = QSharedPointer<LuaCallbackEngine>::create();
	auto              target   = QSharedPointer<LuaCallbackEngine>::create();
	auto              follower = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, target, QStringLiteral(R"lua(
broadcast_line = ""
function read_nested_line()
  local value = GetLineInfo(20, 1)
  Note("target call output")
  return value
end
function OnPluginBroadcast(message, sender_id, sender_name, text)
  broadcast_line = GetLineInfo(300, 1)
  Note("target broadcast output")
end
function nested_dispatch_status(value)
  return broadcast_line
end
)lua"),
	                       &runtime);
	initializeWorkerEngine(executor, follower, QStringLiteral(R"lua(
follower_broadcast_text = ""
function OnPluginBroadcast(message, sender_id, sender_name, text)
  follower_broadcast_text = string.format("%s|%s", GetLineInfo(300, 1), text)
end
function follower_dispatch_status(value)
  return follower_broadcast_text
end
)lua"),
	                       &runtime);
	initializeWorkerEngine(executor, caller, QStringLiteral(R"lua(
nested_dispatch_result = ""
function nested_dispatch_cb(name, line, wildcards)
  local before = GetLineInfo(20, 1)
  Note("first callback output")
  local code, call_line = CallPlugin("Target.Id", "read_nested_line")
  local caller_call_line = GetLineInfo(20, 1)
  Note("second callback output")
  local delivered = BroadcastPlugin(42, "refresh")
  local caller_broadcast_line = GetLineInfo(300, 1)
  nested_dispatch_result = string.format("%s|%.0f|%s|%s|%.0f|%s",
    before, code, call_line, caller_call_line, delivered, caller_broadcast_line)
end
function nested_dispatch_caller_status(value)
  return nested_dispatch_result
end
)lua"),
	                       &runtime);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                            = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration           = generation;
	lineSnapshot->lineBufferCount                = count;
	auto snapshot                                = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot              = true;
	snapshot->lineBufferCount                    = count;
	snapshot->lineBufferSnapshot                 = lineSnapshot;
	snapshot->hasCallbackOutputAnchor            = true;
	snapshot->callbackOutputAnchorBufferIndex    = count;
	snapshot->callbackOutputAnchorAbsoluteNumber = count;
	snapshot->hasWorldAttributeSnapshot          = true;
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("max_output_lines"), QStringLiteral("400"));
	const QString targetKey     = QStringLiteral("target.id");
	snapshot->pluginIdsSnapshot = {targetKey};
	snapshot->pluginIdsByLookupKey.insert(targetKey, targetKey);
	snapshot->pluginNamesById.insert(targetKey, QStringLiteral("Target Plugin"));
	snapshot->pluginEnabledById.insert(targetKey, true);
	snapshot->pluginEnginesById.insert(targetKey, target);
	snapshot->hasBroadcastPluginSnapshot     = true;
	snapshot->broadcastPluginIdsSnapshot     = {targetKey, QStringLiteral("follower.id")};
	snapshot->broadcastPluginEnginesSnapshot = {target, follower};

	LuaBatchDispatchRequest request;
	request.engines               = {caller};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("nested_dispatch_cb");
	request.stringListArg         = {QStringLiteral("nested_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 6);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 5);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 400);

	LuaBatchDispatchRequest callerStatus;
	callerStatus.engines      = {caller};
	callerStatus.kind         = LuaBatchDispatchKind::StringInOut;
	callerStatus.functionName = QStringLiteral("nested_dispatch_caller_status");
	callerStatus.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult callerStatusResult;
	dispatchWorkerAndWait(executor, callerStatus, callerStatusResult);
	QCOMPARE(callerStatusResult.stringResult, QStringLiteral("line 20|0|line 21|line 22|2|line 304"));

	LuaBatchDispatchRequest targetStatus;
	targetStatus.engines      = {target};
	targetStatus.kind         = LuaBatchDispatchKind::StringInOut;
	targetStatus.functionName = QStringLiteral("nested_dispatch_status");
	targetStatus.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult targetStatusResult;
	dispatchWorkerAndWait(executor, targetStatus, targetStatusResult);
	QCOMPARE(targetStatusResult.stringResult, QStringLiteral("line 303"));

	LuaBatchDispatchRequest followerStatus;
	followerStatus.engines      = {follower};
	followerStatus.kind         = LuaBatchDispatchKind::StringInOut;
	followerStatus.functionName = QStringLiteral("follower_dispatch_status");
	followerStatus.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult followerStatusResult;
	dispatchWorkerAndWait(executor, followerStatus, followerStatusResult);
	QCOMPARE(followerStatusResult.stringResult, QStringLiteral("line 304|refresh"));

	teardownWorkerEngine(executor, caller);
	teardownWorkerEngine(executor, target);
	teardownWorkerEngine(executor, follower);
}

void tst_LuaCallbackEngine::workerNestedPageWithoutRecentLinesClearsCallerSnapshot()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 100; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);
	runtime.addRecentLine(QStringLiteral("old recent"));
	QObject::connect(&runtime, &WorldRuntime::outputStyledRequested, &runtime,
	                 [&runtime](const QString &text, const QVector<WorldRuntime::StyleSpan> &, const bool,
	                            const bool) { runtime.addLine(text, WorldRuntime::LineNote); });

	auto              caller = QSharedPointer<LuaCallbackEngine>::create();
	auto              target = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, target, QStringLiteral(R"lua(
function refresh_without_recent()
  Note("target output before page")
  return GetLineInfo(20, 1)
end
)lua"),
	                       &runtime);
	initializeWorkerEngine(executor, caller, QStringLiteral(R"lua(
nested_recent_result = ""
function nested_recent_cb(name, line, wildcards)
  local before = GetRecentLines(1)
  local code, value = CallPlugin("Target.Id", "refresh_without_recent")
  local after = GetRecentLines(1)
  nested_recent_result = string.format("%s|%.0f|%s|%s", before, code, value or "<nil>", after)
end
function nested_recent_status(value)
  return nested_recent_result
end
)lua"),
	                       &runtime);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                  = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration = generation;
	lineSnapshot->lineBufferCount      = count;
	auto snapshot                      = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot    = true;
	snapshot->lineBufferCount          = count;
	snapshot->lineBufferSnapshot       = lineSnapshot;
	snapshot->hasRecentLinesSnapshot   = true;
	snapshot->recentLinesSnapshot      = runtime.recentLines();
	const QString targetKey            = QStringLiteral("target.id");
	snapshot->pluginIdsSnapshot        = {targetKey};
	snapshot->pluginIdsByLookupKey.insert(targetKey, targetKey);
	snapshot->pluginNamesById.insert(targetKey, QStringLiteral("Target Plugin"));
	snapshot->pluginEnabledById.insert(targetKey, true);
	snapshot->pluginEnginesById.insert(targetKey, target);

	LuaBatchDispatchRequest request;
	request.engines               = {caller};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("nested_recent_cb");
	request.stringListArg         = {QStringLiteral("nested_recent_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 3);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		if (pageRequests == 1)
			runtime.addRecentLine(QStringLiteral("new recent"));
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 2);

	LuaBatchDispatchRequest status;
	status.engines      = {caller};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("nested_recent_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("old recent|0|line 20|new recent"));

	teardownWorkerEngine(executor, caller);
	teardownWorkerEngine(executor, target);
}

void tst_LuaCallbackEngine::workerEmptyBufferMultilineOutputRefreshesPresentation()
{
	WorldRuntime runtime;
	QObject::connect(&runtime, &WorldRuntime::outputRequested, &runtime,
	                 [&runtime](const QString &text, const bool, const bool)
	                 { runtime.addLine(text, WorldRuntime::LineNote); });
	QObject::connect(&runtime, &WorldRuntime::outputStyledRequested, &runtime,
	                 [&runtime](const QString &text, const QVector<WorldRuntime::StyleSpan> &, const bool,
	                            const bool) { runtime.addLine(text, WorldRuntime::LineNote); });

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
empty_multiline_result = ""
function empty_multiline_cb(name, line, wildcards)
  Note("first\nsecond")
  empty_multiline_result = string.format("%.0f|%s|%s",
    GetLinesInBufferCount(), GetLineInfo(1, 1), GetLineInfo(2, 1))
end
function empty_multiline_status(value)
  return empty_multiline_result
end
)lua"),
	                       &runtime);

	auto lineSnapshot                  = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration = 0;
	lineSnapshot->lineBufferCount      = 0;
	auto snapshot                      = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot    = true;
	snapshot->lineBufferCount          = 0;
	snapshot->lineBufferSnapshot       = lineSnapshot;

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("empty_multiline_cb");
	request.stringListArg         = {QStringLiteral("empty_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 4);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 3);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 2);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("empty_multiline_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("2|first|second"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerActiveIncomingLineOutputRefreshKeepsAppendedLines()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("history"), WorldRuntime::LineOutput);
	runtime.beginIncomingLineLuaContext(QStringLiteral("active"), WorldRuntime::LineOutput, {});
	QVERIFY(runtime.reserveIncomingLineLuaContextInBuffer());

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
active_growth_result = ""
function active_growth_cb(name, line, wildcards)
  Note("first\nsecond")
  local count = GetLinesInBufferCount()
  active_growth_result = string.format("%.0f|%s|%s", count,
    GetLineInfo(count - 1, 1), GetLineInfo(count, 1))
end
function active_growth_status(value)
  return active_growth_result
end
)lua"),
	                       &runtime);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("active_growth_cb");
	request.stringListArg         = {QStringLiteral("active_growth_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = runtime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int resumeCount = 0;
	QVERIFY(completeWorkerSuspensions(executor, engine, runtime, result, resumeCount));
	QVERIFY(resumeCount > 0);
	executeDeferredMutations(result);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("active_growth_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("4|first|second"));
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 4);
	QCOMPARE(runtime.incomingLineLuaContextBufferIndex(), 2);

	runtime.endIncomingLineLuaContext();
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerDirtyOutputSkipsUnusedNestedLinePage()
{
	WorldRuntime runtime;
	QObject::connect(&runtime, &WorldRuntime::outputRequested, &runtime,
	                 [&runtime](const QString &text, const bool, const bool)
	                 { runtime.addLine(text, WorldRuntime::LineNote); });
	QObject::connect(&runtime, &WorldRuntime::outputStyledRequested, &runtime,
	                 [&runtime](const QString &text, const QVector<WorldRuntime::StyleSpan> &, const bool,
	                            const bool) { runtime.addLine(text, WorldRuntime::LineNote); });

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
unused_page_result = ""
function unused_page_cb(name, line, wildcards)
  Note("dirty output")
  CallPlugin("Missing.Plugin", "missing_routine")
  unused_page_result = string.format("%.0f", BroadcastPlugin(7, "no recipients"))
end
function unused_page_status(value)
  return unused_page_result
end
)lua"),
	                       &runtime);

	auto lineSnapshot                    = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration   = 0;
	lineSnapshot->lineBufferCount        = 0;
	auto snapshot                        = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot      = true;
	snapshot->lineBufferCount            = 0;
	snapshot->lineBufferSnapshot         = lineSnapshot;
	snapshot->hasBroadcastPluginSnapshot = true;

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("unused_page_cb");
	request.stringListArg         = {QStringLiteral("unused_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(!result.suspended);
	executeDeferredMutations(result);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 1);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("unused_page_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerAnchoredOutputRefreshesAfterSuspendedBufferGrowth()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
anchored_growth_result = ""
function anchored_growth_cb(name, line, wildcards)
  local before = GetLineInfo(20, 1)
  Note("anchored callback output")
  anchored_growth_result = string.format("%s|%.0f|%s|%s", before,
    GetLinesInBufferCount(), GetLineInfo(401, 1), GetLineInfo(402, 1))
end
function anchored_growth_status(value)
  return anchored_growth_result
end
)lua"),
	                       &runtime);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                            = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration           = generation;
	lineSnapshot->lineBufferCount                = count;
	auto snapshot                                = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot              = true;
	snapshot->lineBufferCount                    = count;
	snapshot->lineBufferSnapshot                 = lineSnapshot;
	snapshot->hasCallbackOutputAnchor            = true;
	snapshot->callbackOutputAnchorBufferIndex    = count;
	snapshot->callbackOutputAnchorAbsoluteNumber = count;

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("anchored_growth_cb");
	request.stringListArg         = {QStringLiteral("growth_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 5);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		executeDeferredMutations(result);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		if (pageRequests == 1)
		{
			for (int lineNumber = 401; lineNumber <= 410; ++lineNumber)
				runtime.addLine(QStringLiteral("external %1").arg(lineNumber), WorldRuntime::LineOutput);
		}
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 3);
	QCOMPARE(runtime.luaContextLinesInBufferCount(), 411);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("anchored_growth_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("line 20|411|anchored callback output|external 401"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerCallPluginPropagatesTargetLinePageSuspension()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              caller = QSharedPointer<LuaCallbackEngine>::create();
	auto              target = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, target, QStringLiteral(R"lua(
function read_older_line()
  return table.concat({ GetLineInfo(300, 1), GetLineInfo(316, 1), GetLinesInBufferCount() }, "|")
end
)lua"),
	                       &runtime);
	initializeWorkerEngine(executor, caller, QStringLiteral(R"lua(
nested_page_result = ""
function nested_page_cb(name, line, wildcards)
  local code, value = CallPlugin("Target.Id", "read_older_line")
  nested_page_result = code == 0 and (value or "<nil>") or "error"
end
function read_self_older_line()
  return GetLineInfo(20, 1)
end
function self_nested_page_cb(name, line, wildcards)
  local code, value = CallPlugin("Plugin.Id", "read_self_older_line")
  nested_page_result = code == 0 and (value or "<nil>") or "error"
end
function nested_page_status(value)
  return nested_page_result
end
)lua"),
	                       &runtime);

	quint64                             lineBufferGeneration = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int lineBufferCount = runtime.luaContextLinePageByBufferIndex(0, 0, lineBufferGeneration,
	                                                                    ignoredEntries, ignoredRecentLines);
	auto      lineSnapshot    = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration = lineBufferGeneration;
	lineSnapshot->lineBufferCount      = lineBufferCount;
	auto snapshot                      = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot    = true;
	snapshot->lineBufferCount          = lineBufferCount;
	snapshot->lineBufferSnapshot       = lineSnapshot;
	const QString targetKey            = QStringLiteral("target.id");
	snapshot->pluginIdsSnapshot        = {targetKey};
	snapshot->pluginIdsByLookupKey.insert(targetKey, targetKey);
	snapshot->pluginNamesById.insert(targetKey, QStringLiteral("Target Plugin"));
	snapshot->pluginEnabledById.insert(targetKey, true);
	snapshot->pluginEnginesById.insert(targetKey, target);

	LuaBatchDispatchRequest request;
	request.engines               = {caller};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("nested_page_cb");
	request.stringListArg         = {QStringLiteral("nested_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult suspended;
	dispatchWorkerAndWait(executor, request, suspended);
	int targetPageRequestCount = 0;
	while (suspended.suspended)
	{
		++targetPageRequestCount;
		QVERIFY(suspended.hasPendingModalStringRequest);
		QVERIFY(suspended.pendingModalStringRequest.internalImmediateResume);
		QVERIFY(suspended.pendingModalStringRequest.beforeRuntimeResumeCallback);
		runtime.addLine(QStringLiteral("nested growth %1").arg(targetPageRequestCount),
		                WorldRuntime::LineOutput);
		suspended.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = suspended.modalResumeId;
		dispatchWorkerAndWait(executor, resume, suspended);
	}
	QCOMPARE(targetPageRequestCount, 2);

	LuaBatchDispatchRequest status;
	status.engines      = {caller};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("nested_page_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("line 300|line 316|402.0"));

	request.functionName = QStringLiteral("self_nested_page_cb");
	LuaBatchDispatchResult selfSuspended;
	dispatchWorkerAndWait(executor, request, selfSuspended);
	int selfPageRequestCount = 0;
	while (selfSuspended.suspended)
	{
		++selfPageRequestCount;
		QVERIFY(selfSuspended.pendingModalStringRequest.internalImmediateResume);
		QVERIFY(selfSuspended.pendingModalStringRequest.beforeRuntimeResumeCallback);
		selfSuspended.pendingModalStringRequest.beforeRuntimeResumeCallback(runtime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = selfSuspended.modalResumeId;
		dispatchWorkerAndWait(executor, resume, selfSuspended);
	}
	QCOMPARE(selfPageRequestCount, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("line 20"));

	teardownWorkerEngine(executor, caller);
	teardownWorkerEngine(executor, target);
}

void tst_LuaCallbackEngine::workerNestedCrossWorldPageKeepsCallerPresentation()
{
	WorldRuntime primaryRuntime;
	primaryRuntime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Primary"));
	for (int lineNumber = 1; lineNumber <= 200; ++lineNumber)
		primaryRuntime.addLine(QStringLiteral("primary %1").arg(lineNumber), WorldRuntime::LineOutput);
	WorldRuntime secondaryRuntime;
	secondaryRuntime.setWorldAttribute(QStringLiteral("id"), QStringLiteral("secondary-id"));
	secondaryRuntime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Secondary"));
	for (int lineNumber = 1; lineNumber <= 150; ++lineNumber)
		secondaryRuntime.addLine(QStringLiteral("secondary %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              caller = QSharedPointer<LuaCallbackEngine>::create();
	auto              target = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, target, QStringLiteral(R"lua(
function read_secondary_line()
  local secondary = GetWorld("Secondary")
  return secondary:GetLineInfo(20, 1)
end
)lua"),
	                       &primaryRuntime);
	initializeWorkerEngine(executor, caller, QStringLiteral(R"lua(
nested_cross_world_result = ""
function nested_cross_world_cb(name, line, wildcards)
  local code, target_line = CallPlugin("Target.Id", "read_secondary_line")
  nested_cross_world_result = string.format("%.0f|%s|%s", code, target_line or "<nil>",
                                             GetLineInfo(20, 1) or "<nil>")
end
function nested_cross_world_status(value)
  return nested_cross_world_result
end
)lua"),
	                       &primaryRuntime);

	auto snapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(
	    *primaryRuntime.luaCallbackSnapshotForBridgedCall());
	LuaCallbackWorldRuntimeSnapshot secondaryWorld;
	secondaryWorld.runtime = &secondaryRuntime;
	secondaryWorld.id      = QStringLiteral("secondary-id");
	secondaryWorld.name    = QStringLiteral("Secondary");
	snapshot->worldRuntimeSnapshot.push_back(secondaryWorld);
	const QString targetKey = QStringLiteral("target.id");
	snapshot->pluginIdsSnapshot.push_back(targetKey);
	snapshot->pluginIdsByLookupKey.insert(targetKey, targetKey);
	snapshot->pluginNamesById.insert(targetKey, QStringLiteral("Target Plugin"));
	snapshot->pluginEnabledById.insert(targetKey, true);
	snapshot->pluginEnginesById.insert(targetKey, target);

	LuaBatchDispatchRequest request;
	request.engines               = {caller};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("nested_cross_world_cb");
	request.stringListArg         = {QStringLiteral("nested_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 4);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(primaryRuntime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {caller};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 3);

	LuaBatchDispatchRequest status;
	status.engines      = {caller};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("nested_cross_world_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0|secondary 20|primary 20"));

	teardownWorkerEngine(executor, caller);
	teardownWorkerEngine(executor, target);
}

void tst_LuaCallbackEngine::workerCallPluginCancellationReleasesTarget()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              caller = QSharedPointer<LuaCallbackEngine>::create();
	auto              target = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, target, QStringLiteral(R"lua(
cancel_target_completed = false
function cancel_target()
  Note("cancelled target output")
  local value = GetLineInfo(20, 1)
  cancel_target_completed = true
  return value
end
function cancel_target_status(value)
  return tostring(cancel_target_completed)
end
)lua"),
	                       &runtime);
	initializeWorkerEngine(executor, caller, QStringLiteral(R"lua(
function cancel_caller(name, line, wildcards)
  CallPlugin("Target.Id", "cancel_target")
end
)lua"),
	                       &runtime);

	{
		quint64                             lineBufferGeneration = 0;
		QHash<int, WorldRuntime::LineEntry> ignoredEntries;
		QStringList                         ignoredRecentLines;
		const int                           lineBufferCount = runtime.luaContextLinePageByBufferIndex(
		    0, 0, lineBufferGeneration, ignoredEntries, ignoredRecentLines);
		auto lineSnapshot                         = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
		lineSnapshot->lineBufferGeneration        = lineBufferGeneration;
		lineSnapshot->lineBufferCount             = lineBufferCount;
		auto snapshot                             = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
		snapshot->hasLineBufferSnapshot           = true;
		snapshot->lineBufferCount                 = lineBufferCount;
		snapshot->lineBufferSnapshot              = lineSnapshot;
		snapshot->hasCallbackOutputAnchor         = true;
		snapshot->callbackOutputAnchorBufferIndex = lineBufferCount;
		snapshot->callbackOutputAnchorAbsoluteNumber = runtime.lines().constLast().lineNumber;
		const QString targetKey                      = QStringLiteral("target.id");
		snapshot->pluginNamesById.insert(targetKey, QStringLiteral("Target Plugin"));
		snapshot->pluginEnabledById.insert(targetKey, true);
		snapshot->pluginEnginesById.insert(targetKey, target);

		LuaBatchDispatchRequest request;
		request.engines               = {caller};
		request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
		request.functionName          = QStringLiteral("cancel_caller");
		request.stringListArg         = {QStringLiteral("cancel_alias"), QStringLiteral("ignored")};
		request.miniWindowSnapshotArg = snapshot;
		LuaBatchDispatchResult suspended;
		dispatchWorkerAndWait(executor, request, suspended);
		QVERIFY(suspended.suspended);
		QVERIFY(!suspended.deferredRuntimeMutationBatches.isEmpty());
		executeDeferredMutations(suspended);
		QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{1});

		LuaBatchDispatchRequest cancel;
		cancel.engines       = {caller};
		cancel.kind          = LuaBatchDispatchKind::CancelSuspendedModalString;
		cancel.modalResumeId = suspended.modalResumeId;
		LuaBatchDispatchResult cancelled;
		dispatchWorkerAndWait(executor, cancel, cancelled);
		QVERIFY(!cancelled.deferredRuntimeMutationBatches.isEmpty());
		executeDeferredMutations(cancelled);
		QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{0});
		QCOMPARE(runtime.lines().constLast().text, QStringLiteral("cancelled target output"));
	}

	LuaBatchDispatchRequest status;
	status.engines      = {target};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("cancel_target_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("false"));

	const QWeakPointer<LuaCallbackEngine> targetWeak = target;
	teardownWorkerEngine(executor, caller);
	teardownWorkerEngine(executor, target);
	status.engines.clear();
	caller.clear();
	target.clear();
	QVERIFY(targetWeak.isNull());
}

void tst_LuaCallbackEngine::workerBroadcastCancellationReleasesTarget()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              caller = QSharedPointer<LuaCallbackEngine>::create();
	auto              target = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, target, QStringLiteral(R"lua(
broadcast_cancel_completed = false
function OnPluginBroadcast(message, sender_id, sender_name, text)
  Note("cancelled broadcast output")
  GetLineInfo(20, 1)
  broadcast_cancel_completed = true
end
function broadcast_cancel_status(value)
  return tostring(broadcast_cancel_completed)
end
)lua"),
	                       &runtime, QStringLiteral("Target.Id"));
	initializeWorkerEngine(executor, caller, QStringLiteral(R"lua(
function broadcast_cancel_caller(name, line, wildcards)
  BroadcastPlugin(7, "cancel")
end
)lua"),
	                       &runtime, QStringLiteral("Caller.Id"));

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                            = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration           = generation;
	lineSnapshot->lineBufferCount                = count;
	auto snapshot                                = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot              = true;
	snapshot->lineBufferCount                    = count;
	snapshot->lineBufferSnapshot                 = lineSnapshot;
	snapshot->hasCallbackOutputAnchor            = true;
	snapshot->callbackOutputAnchorBufferIndex    = count;
	snapshot->callbackOutputAnchorAbsoluteNumber = runtime.lines().constLast().lineNumber;
	snapshot->hasBroadcastPluginSnapshot         = true;
	snapshot->broadcastPluginIdsSnapshot         = {QStringLiteral("target.id")};
	snapshot->broadcastPluginEnginesSnapshot     = {target};

	LuaBatchDispatchRequest request;
	request.engines               = {caller};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("broadcast_cancel_caller");
	request.stringListArg         = {QStringLiteral("cancel_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult suspended;
	dispatchWorkerAndWait(executor, request, suspended);
	QVERIFY(suspended.suspended);
	QVERIFY(!suspended.deferredRuntimeMutationBatches.isEmpty());
	executeDeferredMutations(suspended);
	QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{1});

	LuaBatchDispatchRequest cancel;
	cancel.engines       = {caller};
	cancel.kind          = LuaBatchDispatchKind::CancelSuspendedModalString;
	cancel.modalResumeId = suspended.modalResumeId;
	LuaBatchDispatchResult cancelled;
	dispatchWorkerAndWait(executor, cancel, cancelled);
	QVERIFY(!cancelled.deferredRuntimeMutationBatches.isEmpty());
	executeDeferredMutations(cancelled);
	QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{0});
	QCOMPARE(runtime.lines().constLast().text, QStringLiteral("cancelled broadcast output"));

	LuaBatchDispatchRequest status;
	status.engines      = {target};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("broadcast_cancel_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("false"));

	teardownWorkerEngine(executor, caller);
	teardownWorkerEngine(executor, target);
}

void tst_LuaCallbackEngine::workerResetCancelsSuspendedSelfCallPluginSafely()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function read_self_during_reset()
  return GetLineInfo(20, 1)
end
function suspend_self_during_reset(name, line, wildcards)
  CallPlugin("Plugin.Id", "read_self_during_reset")
end
function reset_status(value)
  return "loaded"
end
)lua"),
	                       &runtime);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                  = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration = generation;
	lineSnapshot->lineBufferCount      = count;
	auto snapshot                      = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot    = true;
	snapshot->lineBufferCount          = count;
	snapshot->lineBufferSnapshot       = lineSnapshot;

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("suspend_self_during_reset");
	request.stringListArg         = {QStringLiteral("reset_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult suspended;
	dispatchWorkerAndWait(executor, request, suspended);
	QVERIFY(suspended.suspended);

	LuaBatchDispatchRequest reset;
	reset.engines = {engine};
	reset.kind    = LuaBatchDispatchKind::ResetAndLoadScript;
	LuaBatchDispatchResult resetResult;
	dispatchWorkerAndWait(executor, reset, resetResult);
	QVERIFY(resetResult.boolResultValid);
	QVERIFY(resetResult.boolResult);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("reset_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("loaded"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerResetCancelsSuspendedCallPluginTarget()
{
	WorldRuntime runtime;
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		runtime.addLine(QStringLiteral("line %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              caller = QSharedPointer<LuaCallbackEngine>::create();
	auto              target = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, target, QStringLiteral(R"lua(
reset_target_completed = false
function reset_target()
  Note("reset target output")
  GetLineInfo(20, 1)
  reset_target_completed = true
end
function reset_target_status(value)
  return tostring(reset_target_completed)
end
)lua"),
	                       &runtime, QStringLiteral("Target.Id"));
	initializeWorkerEngine(executor, caller, QStringLiteral(R"lua(
function reset_caller(name, line, wildcards)
  CallPlugin("Target.Id", "reset_target")
end
)lua"),
	                       &runtime, QStringLiteral("Caller.Id"));

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                            = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration           = generation;
	lineSnapshot->lineBufferCount                = count;
	auto snapshot                                = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot              = true;
	snapshot->lineBufferCount                    = count;
	snapshot->lineBufferSnapshot                 = lineSnapshot;
	snapshot->hasCallbackOutputAnchor            = true;
	snapshot->callbackOutputAnchorBufferIndex    = count;
	snapshot->callbackOutputAnchorAbsoluteNumber = runtime.lines().constLast().lineNumber;
	const QString targetKey                      = QStringLiteral("target.id");
	snapshot->pluginNamesById.insert(targetKey, QStringLiteral("Target Plugin"));
	snapshot->pluginEnabledById.insert(targetKey, true);
	snapshot->pluginEnginesById.insert(targetKey, target);

	LuaBatchDispatchRequest request;
	request.engines               = {caller};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("reset_caller");
	request.stringListArg         = {QStringLiteral("reset_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult suspended;
	dispatchWorkerAndWait(executor, request, suspended);
	QVERIFY(suspended.suspended);
	QVERIFY(!suspended.deferredRuntimeMutationBatches.isEmpty());
	executeDeferredMutations(suspended);
	QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{1});

	LuaBatchDispatchRequest reset;
	reset.engines = {caller};
	reset.kind    = LuaBatchDispatchKind::ResetAndLoadScript;
	LuaBatchDispatchResult resetResult;
	dispatchWorkerAndWait(executor, reset, resetResult);
	QVERIFY(resetResult.boolResultValid);
	QVERIFY(resetResult.boolResult);
	QVERIFY(!resetResult.deferredRuntimeMutationBatches.isEmpty());
	executeDeferredMutations(resetResult);
	QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{0});

	LuaBatchDispatchRequest status;
	status.engines      = {target};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("reset_target_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("false"));

	teardownWorkerEngine(executor, caller);
	teardownWorkerEngine(executor, target);
}

void tst_LuaCallbackEngine::directDestructionCancelsSuspendedCallPluginTarget()
{
	WorldRuntime runtime;
	runtime.addLine(QStringLiteral("anchor"), WorldRuntime::LineOutput);
	auto caller = QSharedPointer<LuaCallbackEngine>::create();
	auto target = QSharedPointer<LuaCallbackEngine>::create();
	caller->setWorldRuntime(&runtime);
	target->setWorldRuntime(&runtime);
	setEngineScript(*target, QStringLiteral(R"lua(
destruction_target_completed = false
function destruction_target()
  Note("destruction target output")
  utils.inputbox("suspend", "target", "")
  destruction_target_completed = true
end
function destruction_target_status(value)
  return tostring(destruction_target_completed)
end
)lua"));
	setEngineScript(*caller, QStringLiteral(R"lua(
function destruction_caller(name, line, wildcards)
  CallPlugin("Target.Id", "destruction_target")
end
)lua"));
	caller->setPluginInfo(QStringLiteral("Caller.Id"), QStringLiteral("Caller"), QStringLiteral("/tmp"));
	target->setPluginInfo(QStringLiteral("Target.Id"), QStringLiteral("Target"), QStringLiteral("/tmp"));
	WorldRuntime::Plugin targetPlugin;
	targetPlugin.attributes.insert(QStringLiteral("id"), QStringLiteral("Target.Id"));
	targetPlugin.attributes.insert(QStringLiteral("name"), QStringLiteral("Target"));
	targetPlugin.lua = target;
	runtime.pluginsMutable().push_back(targetPlugin);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    runtime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                            = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration           = generation;
	lineSnapshot->lineBufferCount                = count;
	auto snapshot                                = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot              = true;
	snapshot->lineBufferCount                    = count;
	snapshot->lineBufferSnapshot                 = lineSnapshot;
	snapshot->hasCallbackOutputAnchor            = true;
	snapshot->callbackOutputAnchorBufferIndex    = count;
	snapshot->callbackOutputAnchorAbsoluteNumber = runtime.lines().constLast().lineNumber;
	snapshot->pluginNamesById.insert(QStringLiteral("target.id"), QStringLiteral("Target"));
	snapshot->pluginEnabledById.insert(QStringLiteral("target.id"), true);
	snapshot->pluginEnginesById.insert(QStringLiteral("target.id"), target);

	LuaExecutorDirect       executor;
	LuaBatchDispatchRequest request;
	request.engines                  = {caller};
	request.kind                     = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName             = QStringLiteral("destruction_caller");
	request.stringListArg            = {QStringLiteral("alias"), QStringLiteral("ignored")};
	request.stringListArg2           = {QStringLiteral("ignored")};
	request.miniWindowSnapshotArg    = snapshot;
	LuaBatchDispatchResult suspended = executor.dispatchBatch(request);
	QVERIFY(suspended.suspended);
	executeDeferredMutations(suspended);
	QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{1});

	request.engines.clear();
	request.miniWindowSnapshotArg.clear();
	snapshot.clear();
	suspended                                        = {};
	const QWeakPointer<LuaCallbackEngine> callerWeak = caller;
	caller.clear();
	QVERIFY(callerWeak.isNull());
	QCOMPARE(runtime.luaCallbackOutputCursorCount(), qsizetype{0});

	LuaBatchDispatchRequest status;
	status.engines                            = {target};
	status.kind                               = LuaBatchDispatchKind::StringInOut;
	status.functionName                       = QStringLiteral("destruction_target_status");
	status.stringArg                          = QStringLiteral("ignored");
	const LuaBatchDispatchResult statusResult = executor.dispatchBatch(status);
	QCOMPARE(statusResult.stringResult, QStringLiteral("false"));
}

void tst_LuaCallbackEngine::workerWorldProxyPagesTargetAndRestoresCallerPresentation()
{
	WorldRuntime primaryRuntime;
	primaryRuntime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Primary"));
	for (int lineNumber = 1; lineNumber <= 400; ++lineNumber)
		primaryRuntime.addLine(QStringLiteral("primary %1").arg(lineNumber), WorldRuntime::LineOutput);
	auto secondaryRuntime = std::make_unique<WorldRuntime>();
	secondaryRuntime->setWorldAttribute(QStringLiteral("id"), QStringLiteral("secondary-id"));
	secondaryRuntime->setWorldAttribute(QStringLiteral("name"), QStringLiteral("Secondary"));
	for (int lineNumber = 1; lineNumber <= 250; ++lineNumber)
		secondaryRuntime->addLine(QStringLiteral("secondary %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
world_page_result = ""
function cross_world_page_cb(name, line, wildcards)
	local secondary = GetWorld("Secondary")
	world_page_result = string.format("%.0f|%.0f|%s|%.0f|%s|%.0f|%s|%.0f|%s",
	  GetInfo(224), secondary:GetInfo(224), secondary:GetAlphaOption("name"),
	  secondary:GetLinesInBufferCount(), secondary:GetLineInfo(20, 1),
	  GetInfo(224), GetAlphaOption("name"), GetLinesInBufferCount(), GetLineInfo(20, 1))
	secondary:Note("secondary proxy output")
end
function closing_world_page_cb(name, line, wildcards)
  local secondary = GetWorld("Secondary")
  world_page_result = string.format("%.0f|%.0f",
	  secondary:GetLinesInBufferCount() or 0, GetLinesInBufferCount())
end
function world_proxy_argument_cb(name, line, wildcards)
  local secondary = GetWorld("Secondary")
  local ansi_ok, ansi = pcall(function() return secondary:ANSI(31) end)
  local stop_ok, stop = pcall(function() return secondary:StopSound() end)
  world_page_result = ansi_ok and ansi == "\27[31m" and stop_ok and type(stop) == "number"
    and "ok" or "failed"
end
function world_proxy_global_cache_cb(name, line, wildcards)
  SetClipboard("caller clipboard")
  local secondary = GetWorld("Secondary")
  secondary:SetClipboard("target clipboard")
  world_page_result = GetClipboard()
end
function world_page_status(value)
  return world_page_result
end
)lua"),
	                       &primaryRuntime);

	quint64                             generation = 0;
	QHash<int, WorldRuntime::LineEntry> ignoredEntries;
	QStringList                         ignoredRecentLines;
	const int                           count =
	    primaryRuntime.luaContextLinePageByBufferIndex(0, 0, generation, ignoredEntries, ignoredRecentLines);
	auto lineSnapshot                    = QSharedPointer<LuaCallbackLineBufferSnapshot>::create();
	lineSnapshot->lineBufferGeneration   = generation;
	lineSnapshot->lineBufferCount        = count;
	auto snapshot                        = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasLineBufferSnapshot      = true;
	snapshot->lineBufferCount            = count;
	snapshot->lineBufferSnapshot         = lineSnapshot;
	snapshot->hasRuntimeCountersSnapshot = true;
	snapshot->runtimeCounterValues.insert(QStringLiteral("outputLineCount"), count);
	snapshot->hasWorldAttributeSnapshot = true;
	snapshot->worldAttributesSnapshot.insert(QStringLiteral("name"), QStringLiteral("Primary"));
	snapshot->hasUiSnapshot = true;
	LuaCallbackWorldRuntimeSnapshot secondaryWorld;
	secondaryWorld.runtime = secondaryRuntime.get();
	secondaryWorld.id      = QStringLiteral("secondary-id");
	secondaryWorld.name    = QStringLiteral("Secondary");
	snapshot->worldRuntimeSnapshot.push_back(secondaryWorld);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("cross_world_page_cb");
	request.stringListArg         = {QStringLiteral("world_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int pageRequests = 0;
	while (result.suspended)
	{
		++pageRequests;
		QVERIFY(pageRequests <= 8);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(primaryRuntime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(pageRequests, 7);
	QVERIFY(!result.deferredRuntimeMutationBatches.isEmpty());
	executeDeferredMutations(result);
	QCOMPARE(secondaryRuntime->luaCallbackOutputCursorCount(), qsizetype{0});
	QCOMPARE(secondaryRuntime->lines().constLast().text, QStringLiteral("secondary proxy output"));

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("world_page_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult,
	         QStringLiteral("400|250|Secondary|250|secondary 20|400|Primary|400|primary 20"));

	request.functionName = QStringLiteral("world_proxy_argument_cb");
	LuaBatchDispatchResult argumentResult;
	dispatchWorkerAndWait(executor, request, argumentResult);
	int argumentResumeRequests = 0;
	while (argumentResult.suspended)
	{
		++argumentResumeRequests;
		QVERIFY(argumentResumeRequests <= 4);
		QVERIFY(argumentResult.pendingModalStringRequest.beforeRuntimeResumeCallback);
		argumentResult.pendingModalStringRequest.beforeRuntimeResumeCallback(primaryRuntime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = argumentResult.modalResumeId;
		dispatchWorkerAndWait(executor, resume, argumentResult);
	}
	QCOMPARE(argumentResumeRequests, 2);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("ok"));

	request.functionName = QStringLiteral("world_proxy_global_cache_cb");
	LuaBatchDispatchResult globalCacheResult;
	dispatchWorkerAndWait(executor, request, globalCacheResult);
	int globalCacheResumeRequests = 0;
	while (globalCacheResult.suspended)
	{
		++globalCacheResumeRequests;
		QVERIFY(globalCacheResumeRequests <= 2);
		QVERIFY(globalCacheResult.pendingModalStringRequest.beforeRuntimeResumeCallback);
		globalCacheResult.pendingModalStringRequest.beforeRuntimeResumeCallback(primaryRuntime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = globalCacheResult.modalResumeId;
		dispatchWorkerAndWait(executor, resume, globalCacheResult);
	}
	QCOMPARE(globalCacheResumeRequests, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("target clipboard"));

	request.functionName = QStringLiteral("closing_world_page_cb");
	LuaBatchDispatchResult closingResult;
	dispatchWorkerAndWait(executor, request, closingResult);
	QVERIFY(closingResult.suspended);
	QVERIFY(closingResult.pendingModalStringRequest.beforeRuntimeResumeCallback);
	secondaryRuntime.reset();
	QVERIFY(snapshot->worldRuntimeSnapshot.constFirst().runtime.isNull());
	closingResult.pendingModalStringRequest.beforeRuntimeResumeCallback(primaryRuntime, QString());
	LuaBatchDispatchRequest closingResume;
	closingResume.engines       = {engine};
	closingResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	closingResume.modalResumeId = closingResult.modalResumeId;
	dispatchWorkerAndWait(executor, closingResume, closingResult);
	QVERIFY(!closingResult.suspended);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0|400"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::workerVanishedRuntimeDoesNotPublishEmptyLinePage()
{
	WorldRuntime primaryRuntime;
	primaryRuntime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Primary"));
	for (int lineNumber = 1; lineNumber <= 100; ++lineNumber)
		primaryRuntime.addLine(QStringLiteral("primary %1").arg(lineNumber), WorldRuntime::LineOutput);
	auto secondaryRuntime = std::make_unique<WorldRuntime>();
	secondaryRuntime->setWorldAttribute(QStringLiteral("id"), QStringLiteral("secondary-id"));
	secondaryRuntime->setWorldAttribute(QStringLiteral("name"), QStringLiteral("Secondary"));
	for (int lineNumber = 1; lineNumber <= 80; ++lineNumber)
		secondaryRuntime->addLine(QStringLiteral("secondary %1").arg(lineNumber), WorldRuntime::LineOutput);

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
vanished_page_result = ""
function vanished_page_cb(name, line, wildcards)
  local secondary = GetWorld("Secondary")
  local secondary_line = secondary:GetLineInfo(20, 1)
  vanished_page_result = tostring(secondary_line == nil) .. "|" ..
                         (GetLineInfo(20, 1) or "<nil>")
end
function vanished_page_status(value)
  return vanished_page_result
end
)lua"),
	                       &primaryRuntime);

	auto snapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(
	    *primaryRuntime.luaCallbackSnapshotForBridgedCall());
	LuaCallbackWorldRuntimeSnapshot secondaryWorld;
	secondaryWorld.runtime = secondaryRuntime.get();
	secondaryWorld.id      = QStringLiteral("secondary-id");
	secondaryWorld.name    = QStringLiteral("Secondary");
	snapshot->worldRuntimeSnapshot.push_back(secondaryWorld);

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("vanished_page_cb");
	request.stringListArg         = {QStringLiteral("world_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(result.suspended);
	QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
	result.pendingModalStringRequest.beforeRuntimeResumeCallback(primaryRuntime, QString());

	LuaBatchDispatchRequest resume;
	resume.engines       = {engine};
	resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	resume.modalResumeId = result.modalResumeId;
	dispatchWorkerAndWait(executor, resume, result);
	QVERIFY(result.suspended);
	QVERIFY(result.pendingModalStringRequest.linePageResult);
	const QSharedPointer<LuaCallbackLinePageResult> vanishedPage =
	    result.pendingModalStringRequest.linePageResult;
	secondaryRuntime.reset();
	QVERIFY(vanishedPage->runtime.isNull());
	QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
	result.pendingModalStringRequest.beforeRuntimeResumeCallback(primaryRuntime, QString());
	resume.modalResumeId = result.modalResumeId;
	dispatchWorkerAndWait(executor, resume, result);
	QVERIFY(!vanishedPage->presentation);

	int callerPageRequests = 0;
	while (result.suspended)
	{
		++callerPageRequests;
		QVERIFY(callerPageRequests <= 2);
		QVERIFY(result.pendingModalStringRequest.beforeRuntimeResumeCallback);
		result.pendingModalStringRequest.beforeRuntimeResumeCallback(primaryRuntime, QString());
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(callerPageRequests, 1);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("vanished_page_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("true|primary 20"));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::notepadMutationReplayKeepsCreateClaimsAttachedAcrossErase()
{
	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("id"), QStringLiteral("world-id"));

	auto makeNotepad = [&runtime](const QString &title)
	{
		LuaCallbackNotepadSnapshot notepad;
		notepad.runtime   = &runtime;
		notepad.worldId   = QStringLiteral("world-id");
		notepad.title     = title;
		notepad.hasEditor = true;
		return notepad;
	};

	QVector<LuaCallbackNotepadSnapshot> presentation{
	    makeNotepad(QStringLiteral("Before")),
	    makeNotepad(QStringLiteral("Duplicate")),
	    makeNotepad(QStringLiteral("DUPLICATE")),
	};
	using Mutation = QMudLuaCallbackNotepadPresentation::Mutation;
	QVector<Mutation> mutations{
	    {Mutation::Kind::Create, &runtime, QStringLiteral("world-id"), QStringLiteral("Duplicate"),
	     QStringLiteral("first"),	                                                                   false},
	    {Mutation::Kind::Close,  &runtime, QStringLiteral("world-id"), QStringLiteral("Before"),    {}, false},
	    {Mutation::Kind::Create, &runtime, QStringLiteral("world-id"), QStringLiteral("DUPLICATE"),
	     QStringLiteral("second"),	                                                                  false},
	};

	QMudLuaCallbackNotepadPresentation::applyMutations(presentation, mutations);

	QVERIFY(mutations.isEmpty());
	QCOMPARE(presentation.size(), 2);
	QCOMPARE(presentation.at(0).text, QStringLiteral("first"));
	QCOMPARE(presentation.at(1).text, QStringLiteral("second"));
	QVERIFY(presentation.at(0).hasText);
	QVERIFY(presentation.at(1).hasText);
}

void tst_LuaCallbackEngine::freshNotepadSnapshotDoesNotReplayFlushedClose()
{
	WorldRuntime runtime;
	runtime.setWorldAttribute(QStringLiteral("id"), QStringLiteral("world-id"));

	LuaCallbackNotepadSnapshot survivor;
	survivor.runtime   = &runtime;
	survivor.worldId   = QStringLiteral("world-id");
	survivor.title     = QStringLiteral("DUPLICATE");
	survivor.hasEditor = true;

	LuaCallbackMiniWindowSnapshot freshSnapshot;
	freshSnapshot.hasNotepadPresentationSnapshot = true;
	freshSnapshot.notepadSnapshot                = {survivor};

	using Mutation = QMudLuaCallbackNotepadPresentation::Mutation;
	QVector<Mutation> mutations{
	    {Mutation::Kind::Close, &runtime, QStringLiteral("world-id"), QStringLiteral("Duplicate"), {}, false},
	};
	QVector<LuaCallbackNotepadSnapshot> presentation;

	QVERIFY(
	    QMudLuaCallbackNotepadPresentation::installFreshSnapshot(presentation, mutations, &freshSnapshot));
	QVERIFY(mutations.isEmpty());
	QCOMPARE(presentation.size(), 1);
	QCOMPARE(presentation.constFirst().title, QStringLiteral("DUPLICATE"));
}

void tst_LuaCallbackEngine::workerNotepadCachesPreserveGlobalAndOwnerLists()
{
	QTemporaryDir qmudHome;
	QVERIFY(qmudHome.isValid());
	MainWindow frame;
	frame.resize(900, 700);
	frame.show();

	WorldRuntime primaryRuntime(&frame);
	primaryRuntime.setStartupDirectory(qmudHome.path());
	primaryRuntime.setWorldAttribute(QStringLiteral("id"), QStringLiteral("primary-id"));
	primaryRuntime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Primary"));
	WorldRuntime secondaryRuntime(&frame);
	secondaryRuntime.setWorldAttribute(QStringLiteral("id"), QStringLiteral("secondary-id"));
	secondaryRuntime.setWorldAttribute(QStringLiteral("name"), QStringLiteral("Secondary"));

	auto *primaryWindow = new WorldChildWindow(QStringLiteral("Primary"));
	primaryWindow->setRuntime(&primaryRuntime);
	frame.addMdiSubWindow(primaryWindow, true);
	primaryWindow->show();
	auto *secondaryWindow = new WorldChildWindow(QStringLiteral("Secondary"));
	secondaryWindow->setRuntime(&secondaryRuntime);
	frame.addMdiSubWindow(secondaryWindow, true);
	secondaryWindow->show();
	QVERIFY(
	    frame.appendToNotepad(QStringLiteral("shared"), QStringLiteral("primary"), false, &primaryRuntime));
	QVERIFY(frame.appendToNotepad(QStringLiteral("Primary B"), QStringLiteral("primary b"), false,
	                              &primaryRuntime));
	QVERIFY(frame.appendToNotepad(QStringLiteral("Shared"), QStringLiteral("secondary"), false,
	                              &secondaryRuntime));
	QCoreApplication::processEvents();

	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
notepad_result = ""
local function joined(all)
  local titles = GetNotepadList(all)
  return titles and table.concat(titles, ",") or "nil"
end
function cross_world_notepad_cache_cb(name, line, wildcards)
  local secondary = GetWorld("Secondary")
  local accepted = secondary:ReplaceNotepad("Secondary New", "new")
  notepad_result = tostring(accepted) .. "|" .. joined(true)
end
function cross_world_pending_notepad_cache_cb(name, line, wildcards)
  SendToNotepad("Caller Pending", "caller")
  local secondary = GetWorld("Secondary")
  local target_all = secondary:GetNotepadList(true)
  AppendToNotepad("Caller Pending", "+tail")
  notepad_result = table.concat({table.concat(target_all or {}, ","),
                                 GetNotepadText("Caller Pending")}, "|")
  CloseNotepad("Caller Pending", false)
end
function cross_world_notepad_geometry_cb(name, line, wildcards)
  local secondary = GetWorld("Secondary")
  local moved = secondary:MoveNotepadWindow("Shared", 30, 40, 1, 1)
  local position = secondary:GetNotepadWindowPosition("Shared")
  notepad_result = table.concat({tostring(moved), position.left, position.top,
                                 position.width, position.height}, "|")
end
function owner_notepad_cache_cb(name, line, wildcards)
  local before_local = joined(false)
  local before_all = joined(true)
  ReplaceNotepad("SHARED", "changed")
  local replaced_local = joined(false)
  local replaced_all = joined(true)
  CloseNotepad("SHARED", false)
  notepad_result = table.concat({before_local, before_all, replaced_local, replaced_all,
                                 joined(false), joined(true)}, "|")
end
function duplicate_notepad_cache_cb(name, line, wildcards)
  SendToNotepad("Dupe", "first")
  SendToNotepad("DUPE", "second")
  local before_local = joined(false)
  local before_all = joined(true)
  local before_text = GetNotepadText("dupe")
  CloseNotepad("dupe", false)
  notepad_result = table.concat({before_local, before_all, before_text,
                                 joined(false), joined(true), GetNotepadText("dupe")}, "|")
end
function lazy_notepad_document_cb(name, line, wildcards)
  notepad_result = tostring(GetNotepadLength("Primary B")) .. "|" .. GetNotepadText("Primary B")
end
function invalid_notepad_mutations_cb(name, line, wildcards)
  notepad_result = table.concat({
    tostring(ActivateNotepad("Missing")),
    tostring(CloseNotepad("Missing", false)),
    tostring(NotepadColour("Missing", "red", "blue")),
    tostring(NotepadFont("Missing", "Sans Serif", 10, 0)),
    tostring(NotepadReadOnly("Missing", true)),
    tostring(NotepadSaveMethod("Missing", 1)),
    tostring(AppendToNotepad("", "body")),
    tostring(ReplaceNotepad("", "body")),
    tostring(SendToNotepad("", "body")),
    tostring(utils.activatenotepad("Missing")),
    tostring(utils.appendtonotepad("", "body", false))
  }, "|")
end
function query_save_notepad_cache_cb(name, line, wildcards)
  local accepted = CloseNotepad("Query Save", true)
  AppendToNotepad("Query Save", "after")
  notepad_result = tostring(accepted) .. "|" .. GetNotepadText("Query Save")
end
function ordered_notepad_mutations_cb(name, line, wildcards)
  SendToNotepad("Ordered", "body")
  local moved = MoveNotepadWindow("Ordered", 30, 40, 1, 1)
  AppendToNotepad("Ordered", "+tail")
  NotepadColour("Ordered", "red", "blue")
  NotepadReadOnly("Ordered", true)
  NotepadSaveMethod("Ordered", 2)
  local position = GetNotepadWindowPosition("Ordered")
  SendToNotepad("No Geometry", "body")
  local unresolved_position = GetNotepadWindowPosition("No Geometry")
  notepad_result = table.concat({tostring(moved), position.left, position.top,
                                 position.width, position.height,
                                 tostring(unresolved_position == nil)}, "|")
end
function overflowing_notepad_move_cb(name, line, wildcards)
  notepad_result = table.concat({
    tostring(MoveNotepadWindow("Ordered", 2147483648, 0, 10, 10)),
    tostring(MoveNotepadWindow("Ordered", -2147483649, 0, 10, 10)),
    tostring(MoveNotepadWindow("Ordered", 0, 2147483648, 10, 10)),
    tostring(MoveNotepadWindow("Ordered", 0, 0, 2147483648, -2147483649))
  }, "|")
end
function disappearing_notepad_move_cb(name, line, wildcards)
  local moved = MoveNotepadWindow("Vanishing", 30, 40, 1, 1)
  local position = GetNotepadWindowPosition("Vanishing")
  notepad_result = table.concat({tostring(moved), tostring(position == nil),
                                 tostring(GetNotepadWindowPosition("Vanishing") == nil)}, "|")
end
function save_notepad_copy_cb(name, line, wildcards)
  local saved = SaveNotepad("Save Source", "copy.txt", false)
  local source, copy = false, false
  for _, title in ipairs(GetNotepadList(false) or {}) do
    source = source or title == "Save Source"
    copy = copy or title == "copy.txt"
  end
  notepad_result = table.concat({tostring(saved), tostring(source), tostring(copy)}, "|")
end
function save_notepad_replace_cb(name, line, wildcards)
  local saved = SaveNotepad("Save Source", "adopted.txt", true)
  local source, adopted = false, false
  for _, title in ipairs(GetNotepadList(false) or {}) do
    source = source or title == "Save Source"
    adopted = adopted or title == "adopted.txt"
  end
  notepad_result = table.concat({tostring(saved), tostring(source), tostring(adopted),
                                 GetNotepadText("adopted.txt"),
                                 GetNotepadText("Save Source")}, "|")
end
function move_then_rename_notepad_cb(name, line, wildcards)
  local moved = MoveNotepadWindow("Rename Geometry", 30, 40, 1, 1)
  local saved = SaveNotepad("Rename Geometry", "renamed-geometry.txt", true)
  local position = GetNotepadWindowPosition("renamed-geometry.txt")
  notepad_result = table.concat({tostring(moved), tostring(saved), position.left, position.top,
                                 position.width, position.height}, "|")
end
function spaced_notepad_titles_cb(name, line, wildcards)
  SendToNotepad("Trim", "plain")
  SendToNotepad(" Trim ", "spaced")
  local before = GetNotepadText("Trim") .. "|" .. GetNotepadText(" Trim ")
  CloseNotepad(" Trim ", false)
  notepad_result = before .. "|" .. GetNotepadText("Trim") .. "|" .. GetNotepadText(" Trim ")
end
function nested_notepad_mutator()
  AppendToNotepad("Nested Shared", "+target")
  SendToNotepad("Nested Created", "created")
  CloseNotepad("Nested Closed", false)
  return true
end
function nested_notepad_cache_cb(name, line, wildcards)
  SendToNotepad("Nested Shared", "caller")
  SendToNotepad("Nested Closed", "closed")
  local code = CallPlugin("Plugin.Id", "nested_notepad_mutator")
  local has_closed = false
  for _, title in ipairs(GetNotepadList(false) or {}) do
    has_closed = has_closed or title == "Nested Closed"
  end
  notepad_result = table.concat({tostring(code), GetNotepadText("Nested Shared"),
                                 GetNotepadText("Nested Created"), tostring(has_closed)}, "|")
end
function nested_partial_notepad_mutator()
  ReplaceNotepad("Partial Target", "target")
  return true
end
function nested_partial_notepad_cb(name, line, wildcards)
  local code = CallPlugin("Plugin.Id", "nested_partial_notepad_mutator")
  local has_primary, has_target = false, false
  for _, title in ipairs(GetNotepadList(false) or {}) do
    has_primary = has_primary or title == "Primary B"
    has_target = has_target or title == "Partial Target"
  end
  notepad_result = table.concat({tostring(code), tostring(has_primary), tostring(has_target)}, "|")
end
function partial_append_notepad_cb(name, line, wildcards)
  local accepted = AppendToNotepad("Primary B", "+tail")
  notepad_result = tostring(accepted) .. "|" .. GetNotepadText("Primary B")
end
function partial_close_notepad_cb(name, line, wildcards)
  local accepted = CloseNotepad("Partial Close", false)
  local found = false
  for _, title in ipairs(GetNotepadList(false) or {}) do
    found = found or title == "Partial Close"
  end
  notepad_result = tostring(accepted) .. "|" .. tostring(found)
end
function unavailable_notepad_refresh_cb(name, line, wildcards)
  notepad_result = tostring(GetNotepadList(false) == nil)
end
function nested_empty_notepad_mutator()
  CloseNotepad("Only Notepad", false)
  return true
end
function nested_empty_notepad_cb(name, line, wildcards)
  local code = CallPlugin("Plugin.Id", "nested_empty_notepad_mutator")
  notepad_result = tostring(code) .. "|" .. tostring(#(GetNotepadList(false) or {}))
end
function nested_vanishing_notepad_reader()
  return GetNotepadText("Nested Vanishing")
end
function nested_vanishing_notepad_cb(name, line, wildcards)
  local code, text = CallPlugin("Plugin.Id", "nested_vanishing_notepad_reader")
  local found = false
  for _, title in ipairs(GetNotepadList(false) or {}) do
    found = found or title == "Nested Vanishing"
  end
  notepad_result = table.concat({tostring(code), text, tostring(found)}, "|")
end
function notepad_cache_status(value)
  return notepad_result
end
)lua"),
	                       &primaryRuntime);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("notepad_cache_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult  statusResult;

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.stringListArg         = {QStringLiteral("alias"), QStringLiteral("line")};
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	QVERIFY(request.miniWindowSnapshotArg);
	const auto primaryBSnapshot = std::ranges::find_if(
	    request.miniWindowSnapshotArg->notepadSnapshot, [](const LuaCallbackNotepadSnapshot &notepad)
	    { return notepad.title == QStringLiteral("Primary B"); });
	QVERIFY(primaryBSnapshot != request.miniWindowSnapshotArg->notepadSnapshot.cend());
	QVERIFY(primaryBSnapshot->hasEditor);
	QVERIFY(!primaryBSnapshot->hasText);
	QVERIFY(primaryBSnapshot->text.isEmpty());

	request.functionName = QStringLiteral("lazy_notepad_document_cb");
	LuaBatchDispatchResult lazyDocumentResult;
	dispatchWorkerAndWait(executor, request, lazyDocumentResult);
	int lazyDocumentResumeCount = 0;
	QVERIFY(completeWorkerSuspensions(executor, engine, primaryRuntime, lazyDocumentResult,
	                                  lazyDocumentResumeCount));
	QCOMPARE(lazyDocumentResumeCount, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("9.0|primary b"));

	request.functionName = QStringLiteral("cross_world_notepad_cache_cb");
	LuaBatchDispatchResult crossWorldResult;
	dispatchWorkerAndWait(executor, request, crossWorldResult);
	int resumeRequests = 0;
	while (crossWorldResult.suspended)
	{
		++resumeRequests;
		QVERIFY(resumeRequests <= 2);
		QVERIFY(crossWorldResult.pendingModalStringRequest.beforeRuntimeResumeCallback);
		crossWorldResult.pendingModalStringRequest.beforeRuntimeResumeCallback(primaryRuntime, QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = crossWorldResult.modalResumeId;
		dispatchWorkerAndWait(executor, resume, crossWorldResult);
	}
	QCOMPARE(resumeRequests, 1);

	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("true|shared,Primary B,Shared,Secondary New"));

	executeDeferredMutations(crossWorldResult);
	QCoreApplication::processEvents();
	request.functionName          = QStringLiteral("cross_world_pending_notepad_cache_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult crossWorldPendingResult;
	dispatchWorkerAndWait(executor, request, crossWorldPendingResult);
	int pendingResumeRequests = 0;
	while (crossWorldPendingResult.suspended)
	{
		++pendingResumeRequests;
		QVERIFY(pendingResumeRequests <= 2);
		QVERIFY(crossWorldPendingResult.pendingModalStringRequest.beforeRuntimeResumeCallback);
		crossWorldPendingResult.pendingModalStringRequest.beforeRuntimeResumeCallback(primaryRuntime,
		                                                                              QString());
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = crossWorldPendingResult.modalResumeId;
		dispatchWorkerAndWait(executor, resume, crossWorldPendingResult);
	}
	QCOMPARE(pendingResumeRequests, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult,
	         QStringLiteral("shared,Primary B,Shared,Secondary New,Caller Pending|caller+tail"));
	executeDeferredMutations(crossWorldPendingResult);
	QCoreApplication::processEvents();

	request.functionName          = QStringLiteral("cross_world_notepad_geometry_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult crossWorldGeometryResult;
	dispatchWorkerAndWait(executor, request, crossWorldGeometryResult);
	int crossWorldGeometryResumeCount = 0;
	QVERIFY(completeWorkerSuspensions(executor, engine, primaryRuntime, crossWorldGeometryResult,
	                                  crossWorldGeometryResumeCount));
	QCOMPARE(crossWorldGeometryResumeCount, 2);
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList crossWorldGeometryParts = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(crossWorldGeometryParts.size(), 5);
	QCOMPARE(crossWorldGeometryParts.at(0), QStringLiteral("true"));
	executeDeferredMutations(crossWorldGeometryResult);
	QCoreApplication::processEvents();
	TextChildWindow *secondarySharedNotepad = nullptr;
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (notepad && notepad->windowTitle() == QStringLiteral("Shared"))
		{
			secondarySharedNotepad = notepad;
			break;
		}
	}
	QVERIFY(secondarySharedNotepad);
	const QRect realizedSecondaryGeometry = secondarySharedNotepad->normalGeometry().isValid()
	                                            ? secondarySharedNotepad->normalGeometry()
	                                            : secondarySharedNotepad->geometry();
	QCOMPARE(crossWorldGeometryParts.at(1).toInt(), realizedSecondaryGeometry.left());
	QCOMPARE(crossWorldGeometryParts.at(2).toInt(), realizedSecondaryGeometry.top());
	QCOMPARE(crossWorldGeometryParts.at(3).toInt(), realizedSecondaryGeometry.width());
	QCOMPARE(crossWorldGeometryParts.at(4).toInt(), realizedSecondaryGeometry.height());

	request.functionName          = QStringLiteral("owner_notepad_cache_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult ownerResult;
	dispatchWorkerAndWait(executor, request, ownerResult);
	QVERIFY(!ownerResult.suspended);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult,
	         QStringLiteral("shared,Primary B|shared,Primary B,Shared,Secondary New|"
	                        "shared,Primary B|shared,Primary B,Shared,Secondary New|"
	                        "Primary B|Primary B,Shared,Secondary New"));

	executeDeferredMutations(ownerResult);
	QCoreApplication::processEvents();
	request.functionName = QStringLiteral("duplicate_notepad_cache_cb");
	request.miniWindowSnapshotArg.reset();
	LuaBatchDispatchResult duplicateResult;
	dispatchWorkerAndWait(executor, request, duplicateResult);
	int duplicateResumeCount = 0;
	QVERIFY(
	    completeWorkerSuspensions(executor, engine, primaryRuntime, duplicateResult, duplicateResumeCount));
	QCOMPARE(duplicateResumeCount, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult,
	         QStringLiteral("Primary B,Dupe,DUPE|Primary B,Shared,Secondary New,Dupe,DUPE|"
	                        "first|Primary B,DUPE|Primary B,Shared,Secondary New,DUPE|second"));

	executeDeferredMutations(duplicateResult);
	QCoreApplication::processEvents();
	TextChildWindow *remainingDuplicate = nullptr;
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (notepad && notepad->windowTitle().compare(QStringLiteral("Dupe"), Qt::CaseInsensitive) == 0)
		{
			remainingDuplicate = notepad;
			break;
		}
	}
	QVERIFY(remainingDuplicate);
	QVERIFY(remainingDuplicate->editor());
	QCOMPARE(remainingDuplicate->editor()->toPlainText(), QStringLiteral("second"));

	request.functionName          = QStringLiteral("ordered_notepad_mutations_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult orderedResult;
	dispatchWorkerAndWait(executor, request, orderedResult);
	int orderedResumeCount = 0;
	QVERIFY(completeWorkerSuspensions(executor, engine, primaryRuntime, orderedResult, orderedResumeCount));
	QCOMPARE(orderedResumeCount, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList orderedPositionResult = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(orderedPositionResult.size(), 6);
	QCOMPARE(orderedPositionResult.at(0), QStringLiteral("true"));
	QCOMPARE(orderedPositionResult.at(5), QStringLiteral("true"));
	executeDeferredMutations(orderedResult);
	QCoreApplication::processEvents();
	TextChildWindow *orderedNotepad = nullptr;
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (notepad && notepad->windowTitle() == QStringLiteral("Ordered"))
		{
			orderedNotepad = notepad;
			break;
		}
	}
	QVERIFY(orderedNotepad);
	QVERIFY(orderedNotepad->editor());
	QCOMPARE(orderedNotepad->editor()->toPlainText(), QStringLiteral("body+tail"));
	const QRect realizedOrderedGeometry = orderedNotepad->normalGeometry().isValid()
	                                          ? orderedNotepad->normalGeometry()
	                                          : orderedNotepad->geometry();
	QCOMPARE(orderedPositionResult.at(1).toInt(), realizedOrderedGeometry.left());
	QCOMPARE(orderedPositionResult.at(2).toInt(), realizedOrderedGeometry.top());
	QCOMPARE(orderedPositionResult.at(3).toInt(), realizedOrderedGeometry.width());
	QCOMPARE(orderedPositionResult.at(4).toInt(), realizedOrderedGeometry.height());
	QVERIFY(orderedNotepad->editor()->isReadOnly());
	QCOMPARE(orderedNotepad->property("save_method").toInt(), 2);
	QCOMPARE(orderedNotepad->editor()->palette().color(QPalette::Text), QColor(Qt::red));
	QCOMPARE(orderedNotepad->editor()->palette().color(QPalette::Base), QColor(Qt::blue));

	request.functionName          = QStringLiteral("nested_notepad_cache_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult nestedNotepadResult;
	dispatchWorkerAndWait(executor, request, nestedNotepadResult);
	QVERIFY(!nestedNotepadResult.suspended);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0.0|caller+target|created|false"));
	executeDeferredMutations(nestedNotepadResult);
	QCoreApplication::processEvents();
	TextChildWindow *nestedSharedNotepad  = nullptr;
	TextChildWindow *nestedCreatedNotepad = nullptr;
	bool             foundNestedClosed    = false;
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (!notepad)
			continue;
		if (notepad->windowTitle() == QStringLiteral("Nested Shared"))
			nestedSharedNotepad = notepad;
		else if (notepad->windowTitle() == QStringLiteral("Nested Created"))
			nestedCreatedNotepad = notepad;
		else if (notepad->windowTitle() == QStringLiteral("Nested Closed"))
			foundNestedClosed = true;
	}
	QVERIFY(nestedSharedNotepad);
	QVERIFY(nestedSharedNotepad->editor());
	QCOMPARE(nestedSharedNotepad->editor()->toPlainText(), QStringLiteral("caller+target"));
	QVERIFY(nestedCreatedNotepad);
	QVERIFY(nestedCreatedNotepad->editor());
	QCOMPARE(nestedCreatedNotepad->editor()->toPlainText(), QStringLiteral("created"));
	QVERIFY(!foundNestedClosed);
	for (TextChildWindow *notepad : {nestedSharedNotepad, nestedCreatedNotepad})
	{
		notepad->setQuerySaveOnClose(false);
		QVERIFY(notepad->close());
	}
	QCoreApplication::processEvents();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

	const auto completeNotepadSnapshot = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	QVERIFY(completeNotepadSnapshot);
	QVERIFY(completeNotepadSnapshot->hasNotepadPresentationSnapshot);
	auto partialNotepadSnapshot =
	    QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(*completeNotepadSnapshot);
	partialNotepadSnapshot->hasUiSnapshot                  = true;
	partialNotepadSnapshot->hasNotepadPresentationSnapshot = false;
	partialNotepadSnapshot->notepadSnapshot.clear();
	request.functionName          = QStringLiteral("nested_partial_notepad_cb");
	request.miniWindowSnapshotArg = partialNotepadSnapshot;
	LuaBatchDispatchResult partialNotepadResult;
	dispatchWorkerAndWait(executor, request, partialNotepadResult);
	int partialNotepadResumeCount = 0;
	QVERIFY(completeWorkerSuspensions(executor, engine, primaryRuntime, partialNotepadResult,
	                                  partialNotepadResumeCount));
	QCOMPARE(partialNotepadResumeCount, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0.0|true|true"));
	executeDeferredMutations(partialNotepadResult);
	QCoreApplication::processEvents();
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (!notepad || notepad->windowTitle() != QStringLiteral("Partial Target"))
			continue;
		notepad->setQuerySaveOnClose(false);
		QVERIFY(notepad->close());
		break;
	}
	QCoreApplication::processEvents();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

	request.functionName          = QStringLiteral("partial_append_notepad_cb");
	request.miniWindowSnapshotArg = partialNotepadSnapshot;
	LuaBatchDispatchResult partialAppendResult;
	dispatchWorkerAndWait(executor, request, partialAppendResult);
	int partialAppendResumeCount = 0;
	QVERIFY(completeWorkerSuspensions(executor, engine, primaryRuntime, partialAppendResult,
	                                  partialAppendResumeCount));
	QCOMPARE(partialAppendResumeCount, 2);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("true|primary b+tail"));
	executeDeferredMutations(partialAppendResult);
	QCoreApplication::processEvents();

	QVERIFY(frame.appendToNotepad(QStringLiteral("Partial Close"), QStringLiteral("body"), false,
	                              &primaryRuntime));
	QCoreApplication::processEvents();
	request.functionName = QStringLiteral("partial_close_notepad_cb");
	LuaBatchDispatchResult partialCloseResult;
	dispatchWorkerAndWait(executor, request, partialCloseResult);
	int partialCloseResumeCount = 0;
	QVERIFY(completeWorkerSuspensions(executor, engine, primaryRuntime, partialCloseResult,
	                                  partialCloseResumeCount));
	QCOMPARE(partialCloseResumeCount, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("1.0|false"));
	executeDeferredMutations(partialCloseResult);
	QCoreApplication::processEvents();

	request.functionName = QStringLiteral("unavailable_notepad_refresh_cb");
	LuaBatchDispatchResult unavailableRefreshResult;
	dispatchWorkerAndWait(executor, request, unavailableRefreshResult);
	QVERIFY(unavailableRefreshResult.suspended);
	QVERIFY(unavailableRefreshResult.hasPendingModalStringRequest);
	LuaBatchDispatchRequest unavailableResume;
	unavailableResume.engines       = {engine};
	unavailableResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	unavailableResume.modalResumeId = unavailableRefreshResult.modalResumeId;
	dispatchWorkerAndWait(executor, unavailableResume, unavailableRefreshResult);
	QVERIFY(!unavailableRefreshResult.suspended);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("true"));

	auto validEmptyNotepadSnapshot           = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	validEmptyNotepadSnapshot->hasUiSnapshot = true;
	validEmptyNotepadSnapshot->hasNotepadPresentationSnapshot = true;
	LuaCallbackNotepadSnapshot onlyNotepad;
	onlyNotepad.runtime   = &primaryRuntime;
	onlyNotepad.worldId   = QStringLiteral("primary-id");
	onlyNotepad.title     = QStringLiteral("Only Notepad");
	onlyNotepad.hasEditor = true;
	validEmptyNotepadSnapshot->notepadSnapshot.push_back(onlyNotepad);
	LuaBatchDispatchRequest emptyNotepadRequest;
	emptyNotepadRequest.engines               = {engine};
	emptyNotepadRequest.kind                  = LuaBatchDispatchKind::NumberAndUtf8StringsCount;
	emptyNotepadRequest.functionName          = QStringLiteral("nested_empty_notepad_cb");
	emptyNotepadRequest.miniWindowSnapshotArg = validEmptyNotepadSnapshot;
	LuaBatchDispatchResult emptyNotepadResult;
	dispatchWorkerAndWait(executor, emptyNotepadRequest, emptyNotepadResult);
	QVERIFY(!emptyNotepadResult.suspended);
	QVERIFY(emptyNotepadResult.countResultValid);
	QCOMPARE(emptyNotepadResult.countResult, 1);
	QVERIFY(emptyNotepadResult.notepadPresentationChanged);
	QVERIFY(emptyNotepadResult.hasNotepadPresentationSnapshot);
	QVERIFY(emptyNotepadResult.notepadPresentationSnapshot.isEmpty());
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0.0|0"));
	executeDeferredMutations(emptyNotepadResult);
	QCoreApplication::processEvents();

	const QRect orderedGeometryBeforeOverflow = realizedOrderedGeometry;
	request.functionName                      = QStringLiteral("overflowing_notepad_move_cb");
	request.miniWindowSnapshotArg             = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult overflowMoveResult;
	dispatchWorkerAndWait(executor, request, overflowMoveResult);
	QVERIFY(!overflowMoveResult.suspended);
	QVERIFY(overflowMoveResult.deferredRuntimeMutationBatches.isEmpty());
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("false|false|false|false"));
	QCOMPARE(orderedNotepad->normalGeometry().isValid() ? orderedNotepad->normalGeometry()
	                                                    : orderedNotepad->geometry(),
	         orderedGeometryBeforeOverflow);

	QVERIFY(
	    frame.appendToNotepad(QStringLiteral("Vanishing"), QStringLiteral("body"), false, &primaryRuntime));
	QCoreApplication::processEvents();
	request.functionName          = QStringLiteral("disappearing_notepad_move_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult disappearingMoveResult;
	dispatchWorkerAndWait(executor, request, disappearingMoveResult);
	QVERIFY(disappearingMoveResult.suspended);
	QVERIFY(disappearingMoveResult.hasPendingModalStringRequest);
	executeDeferredMutations(disappearingMoveResult);
	QCoreApplication::processEvents();
	TextChildWindow *vanishingNotepad = nullptr;
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (notepad && notepad->windowTitle() == QStringLiteral("Vanishing"))
		{
			vanishingNotepad = notepad;
			break;
		}
	}
	QVERIFY(vanishingNotepad);
	vanishingNotepad->setQuerySaveOnClose(false);
	QVERIFY(vanishingNotepad->close());
	QCoreApplication::processEvents();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QString disappearingResumeResult;
	if (disappearingMoveResult.pendingModalStringRequest.guiCallable)
		disappearingResumeResult = disappearingMoveResult.pendingModalStringRequest.guiCallable();
	if (disappearingMoveResult.pendingModalStringRequest.beforeRuntimeResumeCallback)
	{
		disappearingMoveResult.pendingModalStringRequest.beforeRuntimeResumeCallback(
		    primaryRuntime, disappearingResumeResult);
	}
	LuaBatchDispatchRequest disappearingResume;
	disappearingResume.engines       = {engine};
	disappearingResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	disappearingResume.modalResumeId = disappearingMoveResult.modalResumeId;
	disappearingResume.stringArg     = disappearingResumeResult;
	dispatchWorkerAndWait(executor, disappearingResume, disappearingMoveResult);
	QVERIFY(!disappearingMoveResult.suspended);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("true|true|true"));

	QVERIFY(frame.appendToNotepad(QStringLiteral("Nested Vanishing"), QStringLiteral("nested body"), false,
	                              &primaryRuntime));
	QCoreApplication::processEvents();
	request.functionName          = QStringLiteral("nested_vanishing_notepad_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult nestedVanishingResult;
	dispatchWorkerAndWait(executor, request, nestedVanishingResult);
	QVERIFY(nestedVanishingResult.suspended);
	QVERIFY(nestedVanishingResult.hasPendingModalStringRequest);
	TextChildWindow *nestedVanishingNotepad = nullptr;
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (notepad && notepad->windowTitle() == QStringLiteral("Nested Vanishing"))
		{
			nestedVanishingNotepad = notepad;
			break;
		}
	}
	QVERIFY(nestedVanishingNotepad);
	nestedVanishingNotepad->setQuerySaveOnClose(false);
	QVERIFY(nestedVanishingNotepad->close());
	QCoreApplication::processEvents();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QString nestedVanishingResumeResult;
	if (nestedVanishingResult.pendingModalStringRequest.guiCallable)
		nestedVanishingResumeResult = nestedVanishingResult.pendingModalStringRequest.guiCallable();
	if (nestedVanishingResult.pendingModalStringRequest.beforeRuntimeResumeCallback)
	{
		nestedVanishingResult.pendingModalStringRequest.beforeRuntimeResumeCallback(
		    primaryRuntime, nestedVanishingResumeResult);
	}
	LuaBatchDispatchRequest nestedVanishingResume;
	nestedVanishingResume.engines       = {engine};
	nestedVanishingResume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
	nestedVanishingResume.modalResumeId = nestedVanishingResult.modalResumeId;
	nestedVanishingResume.stringArg     = nestedVanishingResumeResult;
	dispatchWorkerAndWait(executor, nestedVanishingResume, nestedVanishingResult);
	QVERIFY(!nestedVanishingResult.suspended);
	QVERIFY(nestedVanishingResult.notepadPresentationChanged);
	QVERIFY(nestedVanishingResult.hasNotepadPresentationSnapshot);
	QVERIFY(std::ranges::none_of(nestedVanishingResult.notepadPresentationSnapshot,
	                             [](const LuaCallbackNotepadSnapshot &notepad)
	                             { return notepad.title == QStringLiteral("Nested Vanishing"); }));
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("0.0||false"));

	request.functionName          = QStringLiteral("spaced_notepad_titles_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult spacedTitleResult;
	dispatchWorkerAndWait(executor, request, spacedTitleResult);
	QVERIFY(!spacedTitleResult.suspended);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("plain|spaced|plain|"));
	executeDeferredMutations(spacedTitleResult);
	QCoreApplication::processEvents();
	bool foundPlainTitle  = false;
	bool foundSpacedTitle = false;
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (!notepad)
			continue;
		foundPlainTitle  = foundPlainTitle || notepad->windowTitle() == QStringLiteral("Trim");
		foundSpacedTitle = foundSpacedTitle || notepad->windowTitle() == QStringLiteral(" Trim ");
	}
	QVERIFY(foundPlainTitle);
	QVERIFY(!foundSpacedTitle);

	const QString copyPath    = QDir(qmudHome.path()).filePath(QStringLiteral("copy.txt"));
	const QString adoptedPath = QDir(qmudHome.path()).filePath(QStringLiteral("adopted.txt"));
	const QString renamedGeometryPath =
	    QDir(qmudHome.path()).filePath(QStringLiteral("renamed-geometry.txt"));
	for (const QString &path : {copyPath, adoptedPath, renamedGeometryPath})
	{
		QFile existing(path);
		QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Truncate));
		QCOMPARE(existing.write("old"), qint64{3});
	}
	QVERIFY(frame.appendToNotepad(QStringLiteral("Save Source"), QStringLiteral("save body"), false,
	                              &primaryRuntime));
	TextChildWindow *saveSourceNotepad = nullptr;
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (notepad && notepad->windowTitle() == QStringLiteral("Save Source"))
		{
			saveSourceNotepad = notepad;
			break;
		}
	}
	QVERIFY(saveSourceNotepad);
	QVERIFY(saveSourceNotepad->editor());
	QVERIFY(saveSourceNotepad->editor()->document());
	saveSourceNotepad->editor()->document()->setModified(true);
	request.functionName          = QStringLiteral("save_notepad_copy_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult saveCopyResult;
	dispatchWorkerAndWait(executor, request, saveCopyResult);
	int saveCopyResumeCount = 0;
	QVERIFY(completeWorkerSuspensions(executor, engine, primaryRuntime, saveCopyResult, saveCopyResumeCount));
	QCOMPARE(saveCopyResumeCount, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("1.0|true|false"));
	QFile savedCopy(copyPath);
	QVERIFY(savedCopy.open(QIODevice::ReadOnly));
	QCOMPARE(savedCopy.readAll(), QByteArray("save body"));
	QVERIFY(saveSourceNotepad->filePath().isEmpty());
	QVERIFY(saveSourceNotepad->editor()->document()->isModified());

	request.functionName          = QStringLiteral("save_notepad_replace_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult saveReplaceResult;
	dispatchWorkerAndWait(executor, request, saveReplaceResult);
	int saveReplaceResumeCount = 0;
	QVERIFY(completeWorkerSuspensions(executor, engine, primaryRuntime, saveReplaceResult,
	                                  saveReplaceResumeCount));
	QCOMPARE(saveReplaceResumeCount, 2);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("1.0|false|true|save body|"));
	QCOMPARE(saveSourceNotepad->windowTitle(), QStringLiteral("adopted.txt"));
	QCOMPARE(saveSourceNotepad->filePath(), adoptedPath);
	QVERIFY(!saveSourceNotepad->editor()->document()->isModified());
	QFile savedReplacement(adoptedPath);
	QVERIFY(savedReplacement.open(QIODevice::ReadOnly));
	QCOMPARE(savedReplacement.readAll(), QByteArray("save body"));

	QVERIFY(frame.appendToNotepad(QStringLiteral("Rename Geometry"), QStringLiteral("geometry body"), false,
	                              &primaryRuntime));
	request.functionName          = QStringLiteral("move_then_rename_notepad_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult renamedGeometryResult;
	dispatchWorkerAndWait(executor, request, renamedGeometryResult);
	int renamedGeometryResumeCount = 0;
	QVERIFY(completeWorkerSuspensions(executor, engine, primaryRuntime, renamedGeometryResult,
	                                  renamedGeometryResumeCount));
	QCOMPARE(renamedGeometryResumeCount, 2);
	dispatchWorkerAndWait(executor, status, statusResult);
	const QStringList renamedGeometryParts = statusResult.stringResult.split(QLatin1Char('|'));
	QCOMPARE(renamedGeometryParts.size(), 6);
	QCOMPARE(renamedGeometryParts.at(0), QStringLiteral("true"));
	QCOMPARE(renamedGeometryParts.at(1), QStringLiteral("1.0"));
	executeDeferredMutations(renamedGeometryResult);
	QCoreApplication::processEvents();
	TextChildWindow *renamedGeometryNotepad = nullptr;
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (notepad && notepad->windowTitle() == QStringLiteral("renamed-geometry.txt"))
		{
			renamedGeometryNotepad = notepad;
			break;
		}
	}
	QVERIFY(renamedGeometryNotepad);
	const QRect realizedRenamedGeometry = renamedGeometryNotepad->normalGeometry().isValid()
	                                          ? renamedGeometryNotepad->normalGeometry()
	                                          : renamedGeometryNotepad->geometry();
	QCOMPARE(renamedGeometryParts.at(2).toInt(), realizedRenamedGeometry.left());
	QCOMPARE(renamedGeometryParts.at(3).toInt(), realizedRenamedGeometry.top());
	QCOMPARE(renamedGeometryParts.at(4).toInt(), realizedRenamedGeometry.width());
	QCOMPARE(renamedGeometryParts.at(5).toInt(), realizedRenamedGeometry.height());
	QCOMPARE(renamedGeometryNotepad->filePath(), renamedGeometryPath);

	QVERIFY(frame.appendToNotepad(QStringLiteral("Query Save"), QStringLiteral("unchanged"), false,
	                              &primaryRuntime));
	request.functionName          = QStringLiteral("query_save_notepad_cache_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult querySaveResult;
	dispatchWorkerAndWait(executor, request, querySaveResult);
	int querySaveResumeCount = 0;
	QVERIFY(
	    completeWorkerSuspensions(executor, engine, primaryRuntime, querySaveResult, querySaveResumeCount));
	QCOMPARE(querySaveResumeCount, 1);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("1.0|after"));
	executeDeferredMutations(querySaveResult);
	QCoreApplication::processEvents();
	TextChildWindow *recreatedQuerySave = nullptr;
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (notepad && notepad->windowTitle() == QStringLiteral("Query Save"))
		{
			recreatedQuerySave = notepad;
			break;
		}
	}
	QVERIFY(recreatedQuerySave);
	QVERIFY(recreatedQuerySave->editor());
	QCOMPARE(recreatedQuerySave->editor()->toPlainText(), QStringLiteral("after"));

	request.functionName          = QStringLiteral("invalid_notepad_mutations_cb");
	request.miniWindowSnapshotArg = primaryRuntime.luaCallbackSnapshotForBridgedCall();
	LuaBatchDispatchResult invalidMutationResult;
	dispatchWorkerAndWait(executor, request, invalidMutationResult);
	QVERIFY(!invalidMutationResult.suspended);
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult,
	         QStringLiteral("false|0.0|0.0|0.0|0.0|0.0|false|false|false|false|false"));
	executeDeferredMutations(invalidMutationResult);
	QCoreApplication::processEvents();

	teardownWorkerEngine(executor, engine);
	for (TextChildWindow *notepad : frame.notepadWindows())
	{
		if (notepad)
			notepad->setQuerySaveOnClose(false);
	}
	primaryWindow->setRuntime(nullptr);
	secondaryWindow->setRuntime(nullptr);
}

void tst_LuaCallbackEngine::workerUnavailableNotepadRefreshDoesNotExportStaleCaches()
{
	WorldRuntime      runtime;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
unavailable_notepad_result = ""
function read_nested_notepad_list()
  local titles = GetNotepadList(true)
  return titles and table.concat(titles, ",") or "<nil>"
end
function unavailable_notepad_nested_cb(name, line, wildcards)
  local outer_titles = GetNotepadList(true)
  local code, nested_titles = CallPlugin("Plugin.Id", "read_nested_notepad_list")
  unavailable_notepad_result = table.concat({
    tostring(outer_titles == nil), tostring(code == 0), nested_titles or "<nil>"
  }, "|")
end
function unavailable_notepad_status(value)
  return unavailable_notepad_result
end
)lua"),
	                       &runtime);

	auto snapshot                            = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasUiSnapshot                  = true;
	snapshot->hasNotepadPresentationSnapshot = false;
	snapshot->notepadListByKey.insert(QStringLiteral("1"), QStringList{QStringLiteral("stale notepad")});

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::StringsAndWildcards;
	request.functionName          = QStringLiteral("unavailable_notepad_nested_cb");
	request.stringListArg         = {QStringLiteral("notepad_alias"), QStringLiteral("ignored")};
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	int unavailableRefreshes = 0;
	while (result.suspended)
	{
		++unavailableRefreshes;
		QVERIFY(unavailableRefreshes <= 2);
		QVERIFY(result.pendingModalStringRequest.internalImmediateResume);
		LuaBatchDispatchRequest resume;
		resume.engines       = {engine};
		resume.kind          = LuaBatchDispatchKind::ResumeSuspendedModalString;
		resume.modalResumeId = result.modalResumeId;
		dispatchWorkerAndWait(executor, resume, result);
	}
	QCOMPARE(unavailableRefreshes, 2);

	LuaBatchDispatchRequest status;
	status.engines      = {engine};
	status.kind         = LuaBatchDispatchKind::StringInOut;
	status.functionName = QStringLiteral("unavailable_notepad_status");
	status.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult statusResult;
	dispatchWorkerAndWait(executor, status, statusResult);
	QCOMPARE(statusResult.stringResult, QStringLiteral("true|true|<nil>"));

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

void tst_LuaCallbackEngine::callbackMiniWindowResourceIdentityIsExact()
{
	WorldRuntime      runtime;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
		identity_results = ""
		function OnPluginEnable()
		  identity_results = table.concat({
		    tostring(WindowDeleteHotspot("Map", "move")),
		    tostring(WindowDeleteHotspot("Map", "Move  ")),
		    tostring(WindowDeleteHotspot("map|left", "drag")),
		    tostring(WindowText("Map", "font", "x", 0, 0, 10, 10, 0)),
		    tostring(WindowText("Map", "Font  ", "x", 0, 0, 10, 10, 0)),
		    tostring(WindowText("map|left", "font", "x", 0, 0, 10, 10, 0))
		  }, ":")
		end
		function identity_status(value)
		  return identity_results
		end
		)lua"),
	                       &runtime);

	auto snapshot         = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->windowNames = {QStringLiteral("Map"), QStringLiteral("map"), QStringLiteral("map|left")};
	snapshot->fontIdsByWindow.insert(QStringLiteral("Map"),
	                                 {QStringLiteral("Font"), QStringLiteral("Font ")});
	snapshot->fontIdsByWindow.insert(QStringLiteral("map"), {QStringLiteral("left|font")});
	snapshot->hotspotIdsByWindow.insert(QStringLiteral("Map"),
	                                    {QStringLiteral("Move"), QStringLiteral("Move ")});
	snapshot->hotspotIdsByWindow.insert(QStringLiteral("map"), {QStringLiteral("left|drag")});
	snapshot->rebuildMiniWindowLookupCaches();

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(result.hasFunction);
	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("identity_status");
	request.stringArg    = QStringLiteral("ignored");
	request.miniWindowSnapshotArg.reset();
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("%1.0:%1.0:%1.0:-2.0:-2.0:-2.0").arg(eHotspotNotInstalled));

	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::callbackMiniWindowStructuredCachesKeepDelimiterDistinctKeys()
{
	WorldRuntime      runtime;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
function OnPluginEnable()
  assert(WindowInfo("Map", 3) == 101)
  assert(WindowInfo("map", 3) == 202)

  assert(WindowImageInfo("image-a", "b|c", 2) == 3)
  assert(WindowImageInfo("image-a|b", "c", 2) == 7)
  assert(WindowCreateImage("image-a", "b|c", 0, 0, 0, 0, 0, 0, 0, 0) == 0)
  assert(WindowImageInfo("image-a", "b|c", 2) == 8)
  assert(WindowImageInfo("image-a|b", "c", 2) == 7)
  structured_cache_result = "ok"
end
function structured_cache_status(value)
  return structured_cache_result or ""
end
)lua"),
	                       &runtime);

	auto snapshot         = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->windowNames = {QStringLiteral("Map"), QStringLiteral("map"), QStringLiteral("image-a"),
	                         QStringLiteral("image-a|b")};
	LuaCallbackMiniWindowSnapshot::WindowInfoSnapshot upperInfo;
	upperInfo.width = 101;
	LuaCallbackMiniWindowSnapshot::WindowInfoSnapshot lowerInfo;
	lowerInfo.width = 202;
	snapshot->windowInfoByWindow.insert(QStringLiteral("Map"), upperInfo);
	snapshot->windowInfoByWindow.insert(QStringLiteral("map"), lowerInfo);

	const auto makeImageWindow = [](const QString &windowName, const QString &imageId, const int imageWidth)
	{
		auto window = QSharedPointer<MiniWindow>::create();
		MiniWindowUtils::create(*window, windowName, 0, 0, 20, 20, 0, 0, QColor(Qt::black), QString());
		MiniWindowImage image;
		image.image = QImage(imageWidth, 5, QImage::Format_ARGB32);
		image.image.fill(Qt::transparent);
		image.hasAlpha = true;
		window->images.insert(imageId, image);
		return window;
	};
	const auto firstImageWindow  = makeImageWindow(QStringLiteral("image-a"), QStringLiteral("b|c"), 3);
	const auto secondImageWindow = makeImageWindow(QStringLiteral("image-a|b"), QStringLiteral("c"), 7);
	snapshot->miniWindowsByWindow.insert(QStringLiteral("image-a"), firstImageWindow);
	snapshot->miniWindowsByWindow.insert(QStringLiteral("image-a|b"), secondImageWindow);
	snapshot->imageIdsByWindow.insert(QStringLiteral("image-a"), {QStringLiteral("b|c")});
	snapshot->imageIdsByWindow.insert(QStringLiteral("image-a|b"), {QStringLiteral("c")});
	snapshot->imageHasAlphaByKey.insert({QStringLiteral("image-a"), QStringLiteral("b|c")}, true);
	snapshot->imageHasAlphaByKey.insert({QStringLiteral("image-a|b"), QStringLiteral("c")}, false);
	QCOMPARE(snapshot->imageHasAlphaByKey.size(), 2);
	snapshot->rebuildMiniWindowLookupCaches();

	LuaBatchDispatchRequest request;
	request.engines               = {engine};
	request.kind                  = LuaBatchDispatchKind::NoArgs;
	request.functionName          = QStringLiteral("OnPluginEnable");
	request.miniWindowSnapshotArg = snapshot;
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QVERIFY(result.hasFunctionValid);
	QVERIFY(result.hasFunction);
	request.kind         = LuaBatchDispatchKind::StringInOut;
	request.functionName = QStringLiteral("structured_cache_status");
	request.stringArg    = QStringLiteral("ignored");
	request.miniWindowSnapshotArg.reset();
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("ok"));

	teardownWorkerEngine(executor, engine);
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

void tst_LuaCallbackEngine::absoluteMiniWindowBoundsRemainConsistentInsideCallback()
{
	WorldRuntime      runtime;
	auto              engine = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
	initializeWorkerEngine(executor, engine, QStringLiteral(R"lua(
observed = ""
function OnResizeMove(flags, hotspot_id)
  WindowResize("win", 240, 500, 0)
  WindowCreate("win", 540, 30, 280, 500, 0, 2, 0)
  WindowPosition("win", 700, 300, 0, 2)
  observed = string.format("%dx%d@%d,%d",
    WindowInfo("win", 3), WindowInfo("win", 4),
    WindowInfo("win", 1), WindowInfo("win", 2))
end
function resize_status(value)
  return observed
end
function OnOrdinaryCallback(flags, hotspot_id)
  WindowResize("win", 240, 200, 0)
end
function OnAtomicRelocate(flags, hotspot_id)
  WindowCreate("win", 300, 30, 280, 180, 0, 2, 0)
end
function OnFullyBlockedPosition(flags, hotspot_id)
  assert(WindowInfo("win", 1) == 540)
  assert(WindowInfo("win", 2) == 30)
  assert(WindowInfo("win", 7) == 0)
  assert(WindowInfo("win", 8) == 2)
  WindowPosition("win", 700, 30, 0, 2)
  return true
end
function OnFullyBlockedResize(flags, hotspot_id)
  assert(WindowInfo("win", 3) == 100)
  assert(WindowInfo("win", 4) == 80)
  WindowResize("win", 140, 80, 0)
  return true
end
function OnExactGeometryNoOps(flags, hotspot_id)
  assert(WindowPosition("win", 540, 30, 0, 2) == 0)
  assert(WindowResize("win", 100, 80, 0) == 0)
  return true
end
function OnTinyScalePosition(flags, hotspot_id)
  WindowPosition("win", 2147483647, 2147483647, 0, 2)
  tiny_scale_observed = string.format("%d,%d", WindowInfo("win", 1), WindowInfo("win", 2))
  return true
end
function tiny_scale_status(value)
  return tiny_scale_observed or ""
end
function OnFullyBlockedLeftTopCreate(flags, hotspot_id)
  assert(WindowInfo("win", 1) == 0)
  assert(WindowInfo("win", 2) == 0)
  assert(WindowInfo("win", 3) == 100)
  assert(WindowInfo("win", 4) == 80)
  WindowCreate("win", -20, -10, 120, 90, 0, 2, 0)
  return true
end
function OnOutsideZeroWidthCreate(flags, hotspot_id)
  assert(WindowCreate("win", 1000, 0, 0, 80, 0, 2, 0) == 0)
  assert(WindowInfo("win", 1) == 640)
  assert(WindowInfo("win", 3) == 0)
  return true
end
)lua"),
	                       &runtime);
	LuaBatchDispatchRequest warmupRequest;
	warmupRequest.engines      = {engine};
	warmupRequest.kind         = LuaBatchDispatchKind::StringInOut;
	warmupRequest.functionName = QStringLiteral("resize_status");
	warmupRequest.stringArg    = QStringLiteral("ignored");
	LuaBatchDispatchResult warmupResult;
	dispatchWorkerAndWait(executor, warmupRequest, warmupResult);
	QCOMPARE(warmupResult.stringResult, QString());
	executeDeferredMutations(warmupResult);

	const QString windowId = QStringLiteral("win");
	QCOMPARE(runtime.windowCreate(windowId, 540, 30, 100, 80, 0, kMiniWindowAbsoluteLocation,
	                              QColor(Qt::black), QString()),
	         eOK);
	const MiniWindow *runtimeWindow = runtime.miniWindow(windowId);
	QVERIFY(runtimeWindow);

	auto snapshot                                   = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create();
	snapshot->hasCommandUiSnapshot                  = false;
	snapshot->commandUiHasView                      = true;
	snapshot->commandUiHasFrameData                 = true;
	snapshot->commandUiOutputClientWidth            = 320;
	snapshot->commandUiOutputClientHeight           = 240;
	snapshot->geometryConstraintDisplayClientWidth  = 320;
	snapshot->geometryConstraintDisplayClientHeight = 240;
	snapshot->commandUiValues.insert(QStringLiteral("hasView"), true);
	snapshot->commandUiValues.insert(QStringLiteral("hasFrameData"), true);
	snapshot->commandUiValues.insert(QStringLiteral("outputClientWidth"), 320);
	snapshot->commandUiValues.insert(QStringLiteral("outputClientHeight"), 240);
	snapshot->geometryConstrainedMiniWindowName = windowId;
	snapshot->absoluteMiniWindowScaleXOver      = 0.5;
	snapshot->absoluteMiniWindowScaleYOver      = 0.5;
	snapshot->windowNames.push_back(windowId);
	snapshot->miniWindowsByWindow.insert(
	    windowId, QSharedPointer<MiniWindow>::create(runtimeWindow->detachedImageCopy()));
	LuaCallbackMiniWindowSnapshot::WindowInfoSnapshot windowInfo;
	windowInfo.locationX                   = runtimeWindow->location.x();
	windowInfo.locationY                   = runtimeWindow->location.y();
	windowInfo.width                       = runtimeWindow->width;
	windowInfo.height                      = runtimeWindow->height;
	windowInfo.position                    = runtimeWindow->position;
	windowInfo.flags                       = runtimeWindow->flags;
	windowInfo.rectLeft                    = runtimeWindow->location.x();
	windowInfo.rectTop                     = runtimeWindow->location.y();
	windowInfo.rectRight                   = runtimeWindow->location.x() + runtimeWindow->width;
	windowInfo.rectBottom                  = runtimeWindow->location.y() + runtimeWindow->height;
	snapshot->windowInfoByWindow[windowId] = windowInfo;
	snapshot->rebuildMiniWindowLookupCaches();

	LuaBatchDispatchRequest request;
	request.engines                 = {engine};
	request.kind                    = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.numberArg1              = 0;
	request.stringArg2              = QStringLiteral("resizer");
	request.miniWindowSnapshotArg   = snapshot;
	request.hasActionSourceOverride = true;
	request.actionSourceOverride    = WorldRuntime::eHotspotCallback;
	request.functionName            = QStringLiteral("OnFullyBlockedPosition");
	LuaBatchDispatchResult blockedPositionResult;
	dispatchWorkerAndWait(executor, request, blockedPositionResult);
	QVERIFY(blockedPositionResult.boolResultValid);
	QVERIFY(blockedPositionResult.boolResult);
	QVERIFY(blockedPositionResult.deferredRuntimeMutationBatches.isEmpty());

	request.functionName = QStringLiteral("OnFullyBlockedResize");
	LuaBatchDispatchResult blockedResizeResult;
	dispatchWorkerAndWait(executor, request, blockedResizeResult);
	QVERIFY(blockedResizeResult.boolResultValid);
	QVERIFY(blockedResizeResult.boolResult);
	QVERIFY(blockedResizeResult.deferredRuntimeMutationBatches.isEmpty());

	request.functionName = QStringLiteral("OnExactGeometryNoOps");
	LuaBatchDispatchResult exactGeometryNoOpResult;
	dispatchWorkerAndWait(executor, request, exactGeometryNoOpResult);
	QVERIFY(exactGeometryNoOpResult.boolResultValid);
	QVERIFY(exactGeometryNoOpResult.boolResult);
	QVERIFY(exactGeometryNoOpResult.deferredRuntimeMutationBatches.isEmpty());

	auto tinyScaleSnapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(*snapshot);
	tinyScaleSnapshot->absoluteMiniWindowScaleXOver = std::numeric_limits<double>::min();
	tinyScaleSnapshot->absoluteMiniWindowScaleYOver = std::numeric_limits<double>::min();
	request.functionName                            = QStringLiteral("OnTinyScalePosition");
	request.miniWindowSnapshotArg                   = tinyScaleSnapshot;
	LuaBatchDispatchResult tinyScalePositionResult;
	dispatchWorkerAndWait(executor, request, tinyScalePositionResult);
	QVERIFY(tinyScalePositionResult.boolResultValid);
	QVERIFY(tinyScalePositionResult.boolResult);
	QVERIFY(!tinyScalePositionResult.deferredRuntimeMutationBatches.isEmpty());

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("tiny_scale_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	LuaBatchDispatchResult tinyScaleStatusResult;
	dispatchWorkerAndWait(executor, request, tinyScaleStatusResult);
	QCOMPARE(tinyScaleStatusResult.stringResult, QStringLiteral("%1,%2")
	                                                 .arg(std::numeric_limits<int>::max() - 100)
	                                                 .arg(std::numeric_limits<int>::max() - 80));

	request.kind                    = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.functionName            = QStringLiteral("OnResizeMove");
	request.numberArg1              = 0;
	request.stringArg2              = QStringLiteral("resizer");
	request.miniWindowSnapshotArg   = snapshot;
	request.hasActionSourceOverride = true;
	request.actionSourceOverride    = WorldRuntime::eHotspotCallback;
	LuaBatchDispatchResult moveResult;
	dispatchWorkerAndWait(executor, request, moveResult);
	QVERIFY(moveResult.boolResultValid);
	QVERIFY(!moveResult.boolResult);
	executeDeferredMutations(moveResult);

	QCOMPARE(runtime.windowInfo(windowId, 1).toInt(), 540);
	QCOMPARE(runtime.windowInfo(windowId, 2).toInt(), 30);
	QCOMPARE(runtime.windowInfo(windowId, 3).toInt(), 100);
	QCOMPARE(runtime.windowInfo(windowId, 4).toInt(), 450);

	request.kind                  = LuaBatchDispatchKind::StringInOut;
	request.functionName          = QStringLiteral("resize_status");
	request.stringArg             = QStringLiteral("ignored");
	request.miniWindowSnapshotArg = {};
	LuaBatchDispatchResult result;
	dispatchWorkerAndWait(executor, request, result);
	QCOMPARE(result.stringResult, QStringLiteral("100x450@540,30"));

	request.kind                    = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.functionName            = QStringLiteral("OnAtomicRelocate");
	request.numberArg1              = 0;
	request.stringArg2              = QStringLiteral("resizer");
	request.miniWindowSnapshotArg   = snapshot;
	request.hasActionSourceOverride = true;
	request.actionSourceOverride    = WorldRuntime::eHotspotCallback;
	LuaBatchDispatchResult relocateResult;
	dispatchWorkerAndWait(executor, request, relocateResult);
	QVERIFY(relocateResult.boolResultValid);
	executeDeferredMutations(relocateResult);
	QCOMPARE(runtime.windowInfo(windowId, 1).toInt(), 300);
	QCOMPARE(runtime.windowInfo(windowId, 2).toInt(), 30);
	QCOMPARE(runtime.windowInfo(windowId, 3).toInt(), 280);
	QCOMPARE(runtime.windowInfo(windowId, 4).toInt(), 180);

	auto unscopedHotspotSnapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(*snapshot);
	unscopedHotspotSnapshot->geometryConstrainedMiniWindowName.clear();
	request.kind                    = LuaBatchDispatchKind::NumberAndStringStopOnTrue;
	request.functionName            = QStringLiteral("OnOrdinaryCallback");
	request.numberArg1              = 0;
	request.stringArg2              = QStringLiteral("ordinary-hotspot");
	request.miniWindowSnapshotArg   = unscopedHotspotSnapshot;
	request.hasActionSourceOverride = true;
	request.actionSourceOverride    = WorldRuntime::eHotspotCallback;
	LuaBatchDispatchResult ordinaryResult;
	dispatchWorkerAndWait(executor, request, ordinaryResult);
	QVERIFY(ordinaryResult.boolResultValid);
	executeDeferredMutations(ordinaryResult);
	QCOMPARE(runtime.windowInfo(windowId, 3).toInt(), 240);
	QCOMPARE(runtime.windowInfo(windowId, 4).toInt(), 200);

	QCOMPARE(runtime.windowCreate(windowId, 0, 0, 100, 80, 0, kMiniWindowAbsoluteLocation, QColor(Qt::black),
	                              QStringLiteral("Plugin.Id")),
	         eOK);
	runtimeWindow = runtime.miniWindow(windowId);
	QVERIFY(runtimeWindow);
	auto leftTopSnapshot = QSharedPointer<LuaCallbackMiniWindowSnapshot>::create(*snapshot);
	leftTopSnapshot->miniWindowsByWindow[windowId] =
	    QSharedPointer<MiniWindow>::create(runtimeWindow->detachedImageCopy());
	auto &leftTopWindowInfo      = leftTopSnapshot->windowInfoByWindow[windowId];
	leftTopWindowInfo.locationX  = runtimeWindow->location.x();
	leftTopWindowInfo.locationY  = runtimeWindow->location.y();
	leftTopWindowInfo.width      = runtimeWindow->width;
	leftTopWindowInfo.height     = runtimeWindow->height;
	leftTopWindowInfo.position   = runtimeWindow->position;
	leftTopWindowInfo.flags      = runtimeWindow->flags;
	leftTopWindowInfo.rectLeft   = runtimeWindow->location.x();
	leftTopWindowInfo.rectTop    = runtimeWindow->location.y();
	leftTopWindowInfo.rectRight  = runtimeWindow->location.x() + runtimeWindow->width;
	leftTopWindowInfo.rectBottom = runtimeWindow->location.y() + runtimeWindow->height;
	leftTopSnapshot->rebuildMiniWindowLookupCaches();
	request.functionName          = QStringLiteral("OnFullyBlockedLeftTopCreate");
	request.miniWindowSnapshotArg = leftTopSnapshot;
	LuaBatchDispatchResult blockedLeftTopCreateResult;
	dispatchWorkerAndWait(executor, request, blockedLeftTopCreateResult);
	QVERIFY(blockedLeftTopCreateResult.boolResultValid);
	QVERIFY(blockedLeftTopCreateResult.boolResult);
	QVERIFY(blockedLeftTopCreateResult.deferredRuntimeMutationBatches.isEmpty());
	QCOMPARE(runtime.windowInfo(windowId, 1).toInt(), 0);
	QCOMPARE(runtime.windowInfo(windowId, 2).toInt(), 0);
	QCOMPARE(runtime.windowInfo(windowId, 3).toInt(), 100);
	QCOMPARE(runtime.windowInfo(windowId, 4).toInt(), 80);

	request.functionName = QStringLiteral("OnOutsideZeroWidthCreate");
	LuaBatchDispatchResult outsideZeroWidthCreateResult;
	dispatchWorkerAndWait(executor, request, outsideZeroWidthCreateResult);
	QVERIFY(outsideZeroWidthCreateResult.boolResultValid);
	QVERIFY(outsideZeroWidthCreateResult.boolResult);
	QVERIFY(!outsideZeroWidthCreateResult.deferredRuntimeMutationBatches.isEmpty());
	executeDeferredMutations(outsideZeroWidthCreateResult);
	QCOMPARE(runtime.windowInfo(windowId, 1).toInt(), 640);
	QCOMPARE(runtime.windowInfo(windowId, 2).toInt(), 0);
	QCOMPARE(runtime.windowInfo(windowId, 3).toInt(), 0);
	QCOMPARE(runtime.windowInfo(windowId, 4).toInt(), 80);
	teardownWorkerEngine(executor, engine);
}

void tst_LuaCallbackEngine::deferredRuntimeMutationSkipsDestroyedRuntime()
{
	auto              runtime = std::make_unique<WorldRuntime>();
	auto              engine  = QSharedPointer<LuaCallbackEngine>::create();
	LuaExecutorWorker executor(recoveredMutationConsumerForTest());
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
	for (const LuaDeferredRuntimeMutationBatch &batch : result.deferredRuntimeMutationBatches)
		QVERIFY(batch.runtime == runtime.get());
	runtime.reset();
	for (const LuaDeferredRuntimeMutationBatch &batch : result.deferredRuntimeMutationBatches)
	{
		QVERIFY(batch.runtime.isNull());
		for (const std::function<void()> &mutation : batch.mutations)
			mutation();
	}
	teardownWorkerEngine(executor, engine);
}
// NOLINTEND(readability-convert-member-functions-to-static)

QTEST_MAIN(tst_LuaCallbackEngine)

#include "tst_LuaCallbackEngine.moc"
