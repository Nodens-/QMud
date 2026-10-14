/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AtomicSharedPtrCompat.h
 * Role: Cross-platform atomic shared-pointer storage with a macOS libc++ compatibility implementation.
 */

#ifndef QMUD_ATOMICSHAREDPTRCOMPAT_H
#define QMUD_ATOMICSHAREDPTRCOMPAT_H

#include <QtGlobal>
#include <atomic>
#include <memory>
#include <utility>

#ifdef Q_OS_MACOS
/**
 * @brief Provides atomic shared-pointer operations through the API available in macOS libc++.
 * @tparam T Shared object type.
 *
 * As of October 2026, LLVM libc++ does not implement the C++20 `std::atomic<std::shared_ptr<T>>` specialization.
 * Attempting to use it selects the primary `std::atomic<T>` template, which rejects `std::shared_ptr<T>` because
 * it is not trivially copyable. The older atomic shared-pointer free functions remain available in libc++ and
 * provide the same ownership and memory-ordering semantics, so QMud uses them only on macOS while every other
 * platform retains the native C++20 specialization.
 */
template <typename T> class QMudAtomicSharedPtr final
{
	public:
		/** @brief Constructs an empty atomic shared pointer. */
		QMudAtomicSharedPtr() noexcept = default;

		/**
		 * @brief Constructs an atomic shared pointer with an initial value.
		 * @param desired Initial shared pointer.
		 */
		explicit QMudAtomicSharedPtr(std::shared_ptr<T> desired) noexcept : m_value(std::move(desired))
		{
		}

		/** @brief Prevents copying atomic storage. */
		QMudAtomicSharedPtr(const QMudAtomicSharedPtr &) = delete;
		/** @brief Prevents assigning atomic storage. */
		QMudAtomicSharedPtr &operator=(const QMudAtomicSharedPtr &) = delete;

		/**
		 * @brief Atomically loads the current shared pointer.
		 * @param order Memory ordering applied to the load.
		 * @return Shared ownership of the observed value.
		 */
		[[nodiscard]] std::shared_ptr<T>
		load(const std::memory_order order = std::memory_order_seq_cst) const noexcept
		{
			return std::atomic_load_explicit(&m_value, order);
		}

		/**
		 * @brief Atomically replaces the current shared pointer.
		 * @param desired Replacement shared pointer.
		 * @param order Memory ordering applied to the store.
		 */
		void store(std::shared_ptr<T>      desired,
		           const std::memory_order order = std::memory_order_seq_cst) noexcept
		{
			std::atomic_store_explicit(&m_value, std::move(desired), order);
		}

		/**
		 * @brief Atomically replaces and returns the current shared pointer.
		 * @param desired Replacement shared pointer.
		 * @param order Memory ordering applied to the exchange.
		 * @return Shared ownership of the replaced value.
		 */
		[[nodiscard]] std::shared_ptr<T>
		exchange(std::shared_ptr<T>      desired,
		         const std::memory_order order = std::memory_order_seq_cst) noexcept
		{
			return std::atomic_exchange_explicit(&m_value, std::move(desired), order);
		}

	private:
		std::shared_ptr<T> m_value;
};
#else
/**
 * @brief Uses the native C++20 atomic shared-pointer specialization outside macOS.
 * @tparam T Shared object type.
 */
template <typename T> using QMudAtomicSharedPtr = std::atomic<std::shared_ptr<T>>;
#endif

#endif // QMUD_ATOMICSHAREDPTRCOMPAT_H
