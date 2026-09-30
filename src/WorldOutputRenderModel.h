/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldOutputRenderModel.h
 * Role: Runtime-owned logical output rendering state shared by every presentation of one world.
 */

#ifndef QMUD_WORLDOUTPUTRENDERMODEL_H
#define QMUD_WORLDOUTPUTRENDERMODEL_H

#include "IndexedRingBuffer.h"
#include "WorldRuntime.h"

#include <QDateTime>
#include <QString>
// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>

/**
 * @brief Owns the logical output lines shared by all presentations of one world runtime.
 *
 * Layout, selection, scrolling, find, accessibility, and paint state remain view-local. This model contains only
 * the transformation from runtime buffer entries to logical rendered lines and its incremental mutation cursor.
 */
class WorldOutputRenderModel final
{
	public:
		/**
		 * @brief Locates one runtime source entry within a composite logical render line.
		 */
		struct RuntimeSourceSpan
		{
				qint64 lineNumber{0};
				int    prefixStart{0};
				int    prefixLength{0};
				int    textStart{0};
				int    textLength{0};
		};

		/**
		 * @brief One logical render line assembled from one or more runtime entries.
		 */
		struct RenderLine
		{
				QString                          text;
				QVector<WorldRuntime::StyleSpan> spans;
				QDateTime                        fadeTimestamp;
				qint64                           firstRuntimeLineNumber{0};
				qint64                           lastRuntimeLineNumber{0};
				int                              flags{0};
				quint64                          visualHash{0};
				quint64                          sourceRuntimeLineKey{0};
				qint64                           firstRuntimeLineIndex{-1};
				int                              simpleRuntimePrefixLength{0};
				int                              simpleRuntimeTextLength{0};
				QVector<RuntimeSourceSpan>       compositeRuntimeSourceSpans;
		};

		/**
		 * @brief Classifies one revision of the logical render-line model.
		 */
		enum class DeltaKind
		{
			Unknown,
			FullReset,
			TailAppend,
			TailRemove,
			HeadMutation,
			HeadTrimTailAppend,
			RuntimeLineRestitch,
			RuntimeRangeRestitch,
		};

		/**
		 * @brief Identifies the backing source currently represented by the model.
		 */
		enum class Source
		{
			Standalone,
			RuntimeLive,
			RuntimePinned,
		};

		/**
		 * @brief Describes the latest logical render-line revision.
		 */
		struct Delta
		{
				DeltaKind kind{DeltaKind::Unknown};
				quint64   fromRevision{0};
				quint64   revision{0};
				int       oldLineCount{0};
				int       newLineCount{0};
				bool      tailLineMutated{false};
				bool      headLineMutated{false};
				int       headTrimCount{0};
				int       stablePrefixCount{0};
				int       replaceFirstLine{-1};
				int       removedLineCount{0};
				int       insertedLineCount{0};
		};

		using RenderLines = IndexedRingBuffer<RenderLine>;

		RenderLines                      m_nativeRenderLineCache;
		bool                             m_nativeRenderLineCacheValid{false};
		Source                           m_nativeRenderLineCacheSource{Source::Standalone};
		bool                             m_nativeRuntimeTailRestitchPending{false};
		int                              m_nativeRuntimeLineRestitchIndex{-1};
		int                              m_nativeRuntimeRangeRestitchStartIndex{-1};
		int                              m_nativeRuntimeRangeRestitchEndIndexExclusive{-1};
		bool                             m_nativeRuntimeAppendPending{false};
		int                              m_nativeRuntimeFirstAppendedIndex{-1};
		int                              m_nativeRuntimeLastAppendedIndexExclusive{-1};
		int                              m_nativeRuntimeRemovedHeadCountPending{0};
		qint64                           m_nativeRenderRuntimeIndexBase{0};
		quint64                          m_nativeRenderLineCacheRevision{0};
		Delta                            m_nativeRenderCacheDelta;
		bool                             m_nativePartialRenderLineApplied{false};
		bool                             m_nativePartialRenderLineAppended{false};
		bool                             m_nativePartialRenderBaseLastHardReturn{true};
		quint64                          m_nativePartialRenderLineEffectiveRevision{0};
		RenderLine                       m_nativePartialRenderLineBaseTail;
		QString                          m_nativePartialRenderLineText;
		QVector<WorldRuntime::StyleSpan> m_nativePartialRenderLineSpans;
		quint64                          m_nativePartialRenderLineSourceRevision{0};
		bool                             m_nativeHasPartialOutput{false};
		QString                          m_nativePartialOutputText;
		QVector<WorldRuntime::StyleSpan> m_nativePartialOutputSpans;
		quint64                          m_nativePartialOutputRevision{0};
		quint64                          m_lastCompletedPartialRevision{0};
		qint64                           m_lastCompletedPartialRuntimeLineNumber{0};
		int                              m_nativeCachedRuntimeCount{0};
		qint64                           m_nativeCachedRuntimeFirstLineNumber{0};
		qint64                           m_nativeCachedRuntimeLastLineNumber{0};
		bool                             m_nativeCachedRuntimeLastHardReturn{true};
		bool                             m_nativeCachedRuntimeLineNumbersContiguous{true};
		WorldRuntime::LineEntry          m_nativeCachedRuntimeFirstEntry;
		WorldRuntime::LineEntry          m_nativeCachedRuntimeLastEntry;
		quint64                          lastOutputMutationSequence{0};
		quint64                          transformationCount{0};
};

#endif // QMUD_WORLDOUTPUTRENDERMODEL_H
