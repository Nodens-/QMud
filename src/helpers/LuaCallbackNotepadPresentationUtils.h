/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaCallbackNotepadPresentationUtils.h
 * Role: Shared replay helpers for callback-local notepad presentation snapshots.
 */

#pragma once

#include "WorldRuntime.h"

#include <QHash>
#include <QPointer>
#include <QVector>

#include <utility>

/**
 * @brief Maintains callback-local notepad presentation state without replay drift.
 */
namespace QMudLuaCallbackNotepadPresentation
{
	/**
	 * @brief Tests whether a notepad snapshot belongs to a runtime identity.
	 * @param notepad Snapshot whose owner must be matched.
	 * @param runtime Runtime identity preferred when still available.
	 * @param worldId Stable world identifier used when the runtime pointer is unavailable.
	 * @return `true` when the snapshot belongs to the supplied world.
	 */
	inline bool matchesOwner(const LuaCallbackNotepadSnapshot &notepad, const WorldRuntime *runtime,
	                         const QString &worldId)
	{
		if (notepad.runtime == runtime && runtime)
			return true;
		if (notepad.runtime && notepad.runtime != runtime)
			return false;
		return !worldId.isEmpty() && !notepad.worldId.isEmpty() &&
		       notepad.worldId.compare(worldId, Qt::CaseInsensitive) == 0;
	}

	/**
	 * @brief One notepad presentation mutation recorded while no complete snapshot is available.
	 */
	struct Mutation
	{
			/**
			 * @brief Structural or textual operation represented by the mutation.
			 */
			enum class Kind
			{
				Write,  ///< Write or append document contents.
				Create, ///< Create another notepad document.
				Close   ///< Close one matching notepad document.
			};

			Kind                   kind{Kind::Write}; ///< Operation to replay.
			QPointer<WorldRuntime> runtime;           ///< Owning runtime when still alive.
			QString                worldId;           ///< Stable owning-world identifier.
			QString                title;             ///< Case-insensitive notepad title.
			QString                contents;          ///< Text supplied by write/create operations.
			bool                   replace{false};    ///< Replace rather than append on write.
	};

	/**
	 * @brief Installs a presentation captured after deferred UI mutations were flushed.
	 *
	 * The capture already reflects the mutation journal, so retaining or replaying
	 * that journal would apply structural mutations twice.
	 * @param presentation Presentation replaced by the fresh capture.
	 * @param mutations Obsolete mutation journal cleared after installation.
	 * @param snapshot Fresh callback snapshot containing the notepad presentation.
	 * @return `true` when a notepad presentation was available.
	 */
	inline bool installFreshSnapshot(QVector<LuaCallbackNotepadSnapshot> &presentation,
	                                 QVector<Mutation> &mutations, const LuaCallbackSnapshot *const snapshot)
	{
		presentation.clear();
		const bool available = snapshot && snapshot->hasNotepadPresentationSnapshot;
		if (available)
			presentation = snapshot->notepadSnapshot;
		mutations.clear();
		return available;
	}

	/**
	 * @brief Replays deferred notepad mutations onto a presentation snapshot.
	 *
	 * Create claims are stored with their presentation records so erasing or moving
	 * another record cannot transfer a claim to a different notepad.
	 * @param presentation Presentation to update in mutation order.
	 * @param mutations Mutation journal consumed and cleared by the replay.
	 */
	inline void applyMutations(QVector<LuaCallbackNotepadSnapshot> &presentation,
	                           QVector<Mutation>                   &mutations)
	{
		/**
		 * @brief Couples one replay snapshot with its create-claim state.
		 */
		struct ReplayNotepad
		{
				LuaCallbackNotepadSnapshot presentation;
				bool                       claimedByCreate{false};
		};

		auto mutationKey = [](const Mutation &mutation)
		{
			return QStringLiteral("%1|%2|%3")
			    .arg(reinterpret_cast<quintptr>(mutation.runtime.data()), 0, 16)
			    .arg(mutation.worldId.toLower(), mutation.title.toLower());
		};

		QVector<ReplayNotepad> replayNotepads;
		replayNotepads.reserve(presentation.size());
		for (LuaCallbackNotepadSnapshot &notepad : presentation)
			replayNotepads.push_back({std::move(notepad), false});
		presentation.clear();

		auto findPresentationIndex =
		    [&replayNotepads](const QString &title, const WorldRuntime *runtime, const QString &worldId)
		{
			for (qsizetype index = 0; index < replayNotepads.size(); ++index)
			{
				const LuaCallbackNotepadSnapshot &notepad = replayNotepads.at(index).presentation;
				if (notepad.title.compare(title, Qt::CaseInsensitive) == 0 &&
				    matchesOwner(notepad, runtime, worldId))
				{
					return index;
				}
			}
			return qsizetype{-1};
		};

		QHash<QString, int> remainingCreatesByKey;
		for (const Mutation &mutation : std::as_const(mutations))
		{
			if (mutation.kind == Mutation::Kind::Create)
				++remainingCreatesByKey[mutationKey(mutation)];
		}

		for (const Mutation &mutation : std::as_const(mutations))
		{
			WorldRuntime *runtime = mutation.runtime.data();
			qsizetype     index   = findPresentationIndex(mutation.title, runtime, mutation.worldId);
			if (mutation.kind == Mutation::Kind::Close)
			{
				if (index >= 0)
					replayNotepads.removeAt(index);
				continue;
			}

			if (mutation.kind == Mutation::Kind::Create)
			{
				const QString      key       = mutationKey(mutation);
				const int          remaining = remainingCreatesByKey.value(key);
				QVector<qsizetype> candidates;
				for (qsizetype candidate = 0; candidate < replayNotepads.size(); ++candidate)
				{
					const ReplayNotepad              &candidateNotepad = replayNotepads.at(candidate);
					const LuaCallbackNotepadSnapshot &notepad          = candidateNotepad.presentation;
					if (!candidateNotepad.claimedByCreate &&
					    notepad.title.compare(mutation.title, Qt::CaseInsensitive) == 0 &&
					    matchesOwner(notepad, runtime, mutation.worldId))
					{
						candidates.push_back(candidate);
					}
				}
				if (remaining > 0 && candidates.size() >= remaining)
				{
					index                          = candidates.at(candidates.size() - remaining);
					ReplayNotepad &claimed         = replayNotepads[index];
					claimed.claimedByCreate        = true;
					claimed.presentation.text      = mutation.contents;
					claimed.presentation.hasEditor = true;
					claimed.presentation.hasText   = true;
					--remainingCreatesByKey[key];
					continue;
				}
				--remainingCreatesByKey[key];
			}

			if (index < 0)
			{
				LuaCallbackNotepadSnapshot notepad;
				notepad.runtime   = runtime;
				notepad.worldId   = mutation.worldId;
				notepad.title     = mutation.title;
				notepad.text      = mutation.contents;
				notepad.hasEditor = true;
				notepad.hasText   = true;
				replayNotepads.push_back({std::move(notepad), mutation.kind == Mutation::Kind::Create});
				continue;
			}

			LuaCallbackNotepadSnapshot &notepad = replayNotepads[index].presentation;
			if (mutation.replace || !notepad.hasEditor)
			{
				notepad.text    = mutation.contents;
				notepad.hasText = true;
			}
			else if (notepad.hasText)
			{
				notepad.text += mutation.contents;
			}
			notepad.hasEditor = true;
		}

		presentation.reserve(replayNotepads.size());
		for (ReplayNotepad &notepad : replayNotepads)
			presentation.push_back(std::move(notepad.presentation));
		mutations.clear();
	}
} // namespace QMudLuaCallbackNotepadPresentation
