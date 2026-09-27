/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaCallbackContinuation.h
 * Role: Typed, ticketed continuations for runtime operations suspended on Lua callback results.
 */

#ifndef QMUD_LUACALLBACKCONTINUATION_H
#define QMUD_LUACALLBACKCONTINUATION_H

#include "LuaExecutor.h"

#include <QSharedPointer>

#include <concepts>
#include <utility>

/**
 * @brief Whether a result-dependent runtime operation completed inline or was suspended.
 */
enum class LuaCallbackContinuationDisposition
{
	Completed,
	Suspended
};

/**
 * @brief Result of starting a callback dispatch that may suspend its owning runtime operation.
 */
struct LuaCallbackContinuationDispatchResult
{
		LuaCallbackContinuationDisposition disposition{LuaCallbackContinuationDisposition::Completed};
		LuaBatchDispatchResult             result;
		quint64                            ticket{0};

		/**
		 * @brief Returns whether the owning operation was suspended.
		 * @return `true` when completion will arrive through its continuation ticket.
		 */
		[[nodiscard]] bool                 suspended() const noexcept
		{
			return disposition == LuaCallbackContinuationDisposition::Suspended;
		}
};

/**
 * @brief Type-erased continuation owned by a runtime continuation ticket.
 */
class LuaCallbackResultContinuation
{
	public:
		virtual ~LuaCallbackResultContinuation() = default;

		/**
		 * @brief Resumes the owning operation with the completed callback result.
		 * @param result Final callback result.
		 */
		virtual void resume(LuaBatchDispatchResult &&result) = 0;
		/**
		 * @brief Cancels the owning operation with its dispatch fallback result.
		 * @param fallback Dispatch-specific fallback result.
		 */
		virtual void cancel(LuaBatchDispatchResult &&fallback) = 0;
};

/**
 * @brief Lazily constructs a typed continuation only when callback dispatch must suspend.
 */
class LuaCallbackResultContinuationFactory
{
	public:
		virtual ~LuaCallbackResultContinuationFactory() = default;

		/**
		 * @brief Creates the continuation for one suspended operation.
		 * @return Newly owned continuation, or null when creation failed.
		 */
		[[nodiscard]] virtual QSharedPointer<LuaCallbackResultContinuation> create() = 0;
};

/**
 * @brief Continuation implementation that delegates ownership and behavior to a typed state object.
 * @tparam State State type exposing `resume(LuaBatchDispatchResult&&)` and
 * `cancel(LuaBatchDispatchResult&&)`.
 */
template <typename State>
    requires requires(State &state, LuaBatchDispatchResult &&result) {
	    { state.resume(std::move(result)) } -> std::same_as<void>;
	    { state.cancel(std::move(result)) } -> std::same_as<void>;
    }
class LuaCallbackTypedResultContinuation final : public LuaCallbackResultContinuation
{
	public:
		/**
		 * @brief Creates a continuation owning the supplied caller state.
		 * @param state Caller-specific suspension state.
		 */
		explicit LuaCallbackTypedResultContinuation(State state) : m_state(std::move(state))
		{
		}

		void resume(LuaBatchDispatchResult &&result) override
		{
			m_state.resume(std::move(result));
		}

		void cancel(LuaBatchDispatchResult &&fallback) override
		{
			m_state.cancel(std::move(fallback));
		}

	private:
		State m_state;
};

/**
 * @brief Stack-owned lazy factory for a caller-specific continuation.
 * @tparam StateFactory Callable returning either a plain continuation state or a direct
 * `LuaCallbackResultContinuation` implementation.
 */
template <typename StateFactory>
class LuaCallbackTypedResultContinuationFactory final : public LuaCallbackResultContinuationFactory
{
	public:
		using State = std::invoke_result_t<StateFactory &>;

		/**
		 * @brief Creates a lazy factory around a continuation-producing callable.
		 * @param stateFactory Callable invoked only when dispatch suspends.
		 */
		explicit LuaCallbackTypedResultContinuationFactory(StateFactory stateFactory)
		    : m_stateFactory(std::move(stateFactory))
		{
		}

		[[nodiscard]] QSharedPointer<LuaCallbackResultContinuation> create() override
		{
			if constexpr (std::derived_from<State, LuaCallbackResultContinuation>)
				return QSharedPointer<State>::create(m_stateFactory());
			else
				return QSharedPointer<LuaCallbackTypedResultContinuation<State>>::create(m_stateFactory());
		}

	private:
		StateFactory m_stateFactory;
};

/**
 * @brief Builds a stack-owned lazy continuation factory without erasing the factory callable.
 * @tparam StateFactory Callable returning one typed continuation state or implementation.
 * @param stateFactory Caller-specific continuation factory.
 * @return Lazy typed continuation factory.
 */
template <typename StateFactory>
[[nodiscard]] auto makeLuaCallbackResultContinuationFactory(StateFactory &&stateFactory)
{
	return LuaCallbackTypedResultContinuationFactory<std::decay_t<StateFactory>>(
	    std::forward<StateFactory>(stateFactory));
}

#endif // QMUD_LUACALLBACKCONTINUATION_H
