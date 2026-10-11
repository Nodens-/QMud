/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PagedIndexedRingBuffer.h
 * Role: Page-backed indexed sequence for large layout metadata with cheap head removal and stable growth.
 */

#pragma once

// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>
#include <QtGlobal>

#include <array>
#include <deque>
#include <memory>
#include <type_traits>
#include <utility>

/**
 * @brief Page-backed logical sequence with indexed access and cheap head removal.
 *
 * Growth allocates one fixed-size page and never relocates existing elements. Fully
 * consumed head pages rotate to the tail so bounded remove-front/append workloads
 * reuse storage. Middle replacements shift the smaller retained side.
 *
 * @tparam T Stored value type.
 * @tparam PageSize Number of elements in one page; must be a power of two.
 */
template <typename T, qsizetype PageSize = 1024> class PagedIndexedRingBuffer
{
		static_assert(PageSize > 0 && (PageSize & (PageSize - 1)) == 0,
		              "PagedIndexedRingBuffer PageSize must be a power of two");

	public:
		using value_type = T;
		using size_type  = qsizetype;

		/**
		 * @brief Logical range replacement dimensions.
		 */
		struct Replacement
		{
				size_type index{0};
				size_type removeCount{0};
				size_type insertCount{0};
		};

		/**
		 * @brief Constructs an empty paged sequence.
		 */
		PagedIndexedRingBuffer() = default;
		/**
		 * @brief Disallows copying page ownership.
		 */
		PagedIndexedRingBuffer(const PagedIndexedRingBuffer &) = delete;
		/**
		 * @brief Disallows copied assignment of page ownership.
		 * @return This buffer is never returned because copying is disabled.
		 */
		PagedIndexedRingBuffer &operator=(const PagedIndexedRingBuffer &) = delete;
		/**
		 * @brief Moves page ownership from another buffer.
		 */
		PagedIndexedRingBuffer(PagedIndexedRingBuffer &&) noexcept = default;
		/**
		 * @brief Replaces this buffer by moving page ownership from another buffer.
		 * @return This buffer after the move.
		 */
		PagedIndexedRingBuffer &operator=(PagedIndexedRingBuffer &&) noexcept = default;

		/**
		 * @brief Returns the number of logical elements.
		 * @return Logical element count.
		 */
		[[nodiscard]] size_type size() const
		{
			return m_size;
		}

		/**
		 * @brief Returns whether the sequence is empty.
		 * @return `true` when no logical elements are stored.
		 */
		[[nodiscard]] bool isEmpty() const
		{
			return m_size == 0;
		}

		/**
		 * @brief Returns the currently available logical capacity from the head position.
		 * @return Number of elements that fit without allocating another page.
		 */
		[[nodiscard]] size_type capacity() const
		{
			return static_cast<size_type>(m_pages.size()) * PageSize - m_headOffset;
		}

		/**
		 * @brief Releases all pages and resets the sequence.
		 */
		void clear()
		{
			m_pages.clear();
			m_headOffset = 0;
			m_size       = 0;
		}

		/**
		 * @brief Exchanges storage and logical state with another buffer.
		 * @param other Buffer to exchange with.
		 */
		void swap(PagedIndexedRingBuffer &other) noexcept
		{
			m_pages.swap(other.m_pages);
			std::swap(m_headOffset, other.m_headOffset);
			std::swap(m_size, other.m_size);
		}

		/**
		 * @brief Ensures the requested logical capacity without relocating existing elements.
		 * @param requestedCapacity Required logical capacity.
		 */
		void reserve(const size_type requestedCapacity)
		{
			Q_ASSERT(requestedCapacity >= 0);
			ensureTailCapacity(requestedCapacity);
		}

		/**
		 * @brief Changes the logical size, value-initializing newly added elements.
		 * @param newSize Required logical size.
		 */
		void resize(const size_type newSize)
		{
			Q_ASSERT(newSize >= 0);
			if (newSize < m_size)
			{
				remove(newSize, m_size - newSize);
				return;
			}
			ensureTailCapacity(newSize);
			const size_type oldSize = m_size;
			m_size                  = newSize;
			for (size_type index = oldSize; index < newSize; ++index)
				storageAtLogical(index) = T{};
		}

		/**
		 * @brief Shrinks the logical tail without clearing trivially destructible values.
		 * @param newSize Required logical size, bounded by the current size.
		 */
		void truncateBack(const size_type newSize)
		    requires std::is_trivially_destructible_v<T>
		{
			Q_ASSERT(newSize >= 0 && newSize <= m_size);
			m_size = newSize;
			if (m_size == 0)
				m_headOffset = 0;
		}

		/**
		 * @brief Replaces the sequence with repeated copies of a value.
		 * @param count Number of logical elements.
		 * @param value Value assigned to every element.
		 */
		void assign(const size_type count, const T &value)
		{
			Q_ASSERT(count >= 0);
			clear();
			reserve(count);
			m_size = count;
			for (size_type index = 0; index < count; ++index)
				storageAtLogical(index) = value;
		}

		/**
		 * @brief Returns mutable indexed access.
		 * @param index Zero-based logical index.
		 * @return Mutable element reference.
		 */
		[[nodiscard]] T &operator[](const size_type index)
		{
			Q_ASSERT(index >= 0 && index < m_size);
			return storageAtLogical(index);
		}

		/**
		 * @brief Returns immutable indexed access.
		 * @param index Zero-based logical index.
		 * @return Immutable element reference.
		 */
		[[nodiscard]] const T &operator[](const size_type index) const
		{
			Q_ASSERT(index >= 0 && index < m_size);
			return storageAtLogical(index);
		}

		/**
		 * @brief Returns mutable checked indexed access.
		 * @param index Zero-based logical index.
		 * @return Mutable element reference.
		 */
		[[nodiscard]] T &at(const size_type index)
		{
			return (*this)[index];
		}

		/**
		 * @brief Returns immutable checked indexed access.
		 * @param index Zero-based logical index.
		 * @return Immutable element reference.
		 */
		[[nodiscard]] const T &at(const size_type index) const
		{
			return (*this)[index];
		}

		/**
		 * @brief Returns the first mutable element.
		 * @return Mutable first-element reference.
		 */
		[[nodiscard]] T &first()
		{
			return (*this)[0];
		}

		/**
		 * @brief Returns the first immutable element.
		 * @return Immutable first-element reference.
		 */
		[[nodiscard]] const T &constFirst() const
		{
			return (*this)[0];
		}

		/**
		 * @brief Returns the last mutable element.
		 * @return Mutable last-element reference.
		 */
		[[nodiscard]] T &last()
		{
			return (*this)[m_size - 1];
		}

		/**
		 * @brief Returns the last immutable element.
		 * @return Immutable last-element reference.
		 */
		[[nodiscard]] const T &constLast() const
		{
			return (*this)[m_size - 1];
		}

		/**
		 * @brief Appends a copied value.
		 * @param value Value to append.
		 */
		void push_back(const T &value)
		{
			ensureTailCapacity(m_size + 1);
			storageAtLogicalUnchecked(m_size) = value;
			++m_size;
		}

		/**
		 * @brief Appends a moved value.
		 * @param value Value to append.
		 */
		void push_back(T &&value)
		{
			ensureTailCapacity(m_size + 1);
			storageAtLogicalUnchecked(m_size) = std::move(value);
			++m_size;
		}

		/**
		 * @brief Appends a copied value.
		 * @param value Value to append.
		 */
		void append(const T &value)
		{
			push_back(value);
		}

		/**
		 * @brief Removes the last logical element.
		 */
		void removeLast()
		{
			Q_ASSERT(m_size > 0);
			storageAtLogical(m_size - 1) = T{};
			--m_size;
			if (m_size == 0)
				m_headOffset = 0;
		}

		/**
		 * @brief Removes a logical range while preserving order.
		 * @param index First logical element to remove.
		 * @param count Maximum number of elements to remove.
		 */
		void remove(const size_type index, const size_type count = 1)
		{
			if (count <= 0)
				return;
			Q_ASSERT(index >= 0 && index <= m_size);
			const size_type boundedCount = qMin(count, m_size - index);
			if (boundedCount <= 0)
				return;
			replace({index, boundedCount, 0}, T{});
		}

		/**
		 * @brief Replaces one logical range with repeated copies of a value.
		 * @param replacement Logical range and replacement count.
		 * @param value Value assigned to each inserted element.
		 */
		void replace(const Replacement &replacement, const T &value)
		{
			const size_type index       = replacement.index;
			const size_type removeCount = replacement.removeCount;
			const size_type insertCount = replacement.insertCount;
			Q_ASSERT(index >= 0 && index <= m_size);
			Q_ASSERT(removeCount >= 0);
			Q_ASSERT(insertCount >= 0);

			const size_type boundedRemove = qBound<size_type>(0, removeCount, m_size - index);
			const size_type boundedInsert = qMax<size_type>(0, insertCount);
			if (boundedRemove == 0 && boundedInsert == 0)
				return;

			const size_type oldSize     = m_size;
			const size_type suffixFirst = index + boundedRemove;
			const size_type suffixCount = oldSize - suffixFirst;
			const size_type newSize     = oldSize - boundedRemove + boundedInsert;
			if (boundedInsert > boundedRemove)
			{
				const size_type growth = boundedInsert - boundedRemove;
				if (index < suffixCount)
				{
					prependLogicalCapacity(growth);
					m_size = newSize;
					for (size_type source = 0; source < index; ++source)
						storageAtLogical(source) = std::move(storageAtLogical(source + growth));
				}
				else
				{
					ensureTailCapacity(newSize);
					m_size = newSize;
					for (size_type offset = suffixCount; offset > 0; --offset)
					{
						const size_type source            = suffixFirst + offset - 1;
						storageAtLogical(source + growth) = std::move(storageAtLogical(source));
					}
				}
			}
			else if (boundedRemove > boundedInsert)
			{
				const size_type shrink = boundedRemove - boundedInsert;
				if (index < suffixCount)
				{
					for (size_type source = index; source > 0; --source)
						storageAtLogical(source + shrink - 1) = std::move(storageAtLogical(source - 1));
					discardPhysicalFront(shrink);
					m_size = newSize;
				}
				else
				{
					for (size_type offset = 0; offset < suffixCount; ++offset)
						storageAtLogical(index + boundedInsert + offset) =
						    std::move(storageAtLogical(suffixFirst + offset));
					for (size_type tail = newSize; tail < oldSize; ++tail)
						storageAtLogical(tail) = T{};
					m_size = newSize;
				}
			}
			else
			{
				m_size = newSize;
			}

			if (m_size == 0)
			{
				m_headOffset = 0;
				return;
			}
			for (size_type offset = 0; offset < boundedInsert; ++offset)
				storageAtLogical(index + offset) = value;
		}

		/**
		 * @brief Copies logical contents into a contiguous Qt vector.
		 * @return Values in logical order.
		 */
		[[nodiscard]] QVector<T> toVector() const
		{
			QVector<T> result;
			result.reserve(m_size);
			for (size_type index = 0; index < m_size; ++index)
				result.push_back(at(index));
			return result;
		}

	private:
		using Page = std::array<T, static_cast<std::size_t>(PageSize)>;

		/**
		 * @brief Adds tail pages until the requested logical size fits.
		 * @param requestedSize Required logical extent.
		 */
		void ensureTailCapacity(const size_type requestedSize)
		{
			while (m_headOffset + requestedSize > static_cast<size_type>(m_pages.size()) * PageSize)
				m_pages.push_back(std::make_unique<Page>());
		}

		/**
		 * @brief Opens unused logical space before the current head.
		 * @param count Number of logical positions to prepend.
		 */
		void prependLogicalCapacity(const size_type count)
		{
			while (m_headOffset < count)
			{
				m_pages.push_front(std::make_unique<Page>());
				m_headOffset += PageSize;
			}
			m_headOffset -= count;
		}

		/**
		 * @brief Clears and discards physical head positions without changing logical size.
		 * @param count Number of head positions to discard.
		 */
		void discardPhysicalFront(const size_type count)
		{
			for (size_type index = 0; index < count; ++index)
				storageAtLogical(index) = T{};
			m_headOffset += count;
			while (m_headOffset >= PageSize && !m_pages.empty())
			{
				m_headOffset -= PageSize;
				auto page = std::move(m_pages.front());
				m_pages.pop_front();
				m_pages.push_back(std::move(page));
			}
		}

		/**
		 * @brief Returns mutable storage without checking logical size.
		 * @param logicalIndex Logical position within allocated capacity.
		 * @return Mutable element reference.
		 */
		[[nodiscard]] T &storageAtLogicalUnchecked(const size_type logicalIndex)
		{
			const size_type physicalIndex = m_headOffset + logicalIndex;
			const size_type pageIndex     = physicalIndex / PageSize;
			const size_type pageOffset    = physicalIndex & (PageSize - 1);
			return (*m_pages[static_cast<std::size_t>(pageIndex)])[static_cast<std::size_t>(pageOffset)];
		}

		/**
		 * @brief Returns mutable storage at a logical position.
		 * @param logicalIndex Zero-based logical index.
		 * @return Mutable element reference.
		 */
		[[nodiscard]] T &storageAtLogical(const size_type logicalIndex)
		{
			return storageAtLogicalUnchecked(logicalIndex);
		}

		/**
		 * @brief Returns immutable storage at a logical position.
		 * @param logicalIndex Zero-based logical index.
		 * @return Immutable element reference.
		 */
		[[nodiscard]] const T &storageAtLogical(const size_type logicalIndex) const
		{
			const size_type physicalIndex = m_headOffset + logicalIndex;
			const size_type pageIndex     = physicalIndex / PageSize;
			const size_type pageOffset    = physicalIndex & (PageSize - 1);
			return (*m_pages[static_cast<std::size_t>(pageIndex)])[static_cast<std::size_t>(pageOffset)];
		}

		std::deque<std::unique_ptr<Page>> m_pages;
		size_type                         m_headOffset{0};
		size_type                         m_size{0};
};
