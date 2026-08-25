#pragma once

// ReSharper disable once CppUnusedIncludeDirective
#include <QVector>
#include <QtGlobal>

#include <iterator>
#include <type_traits>
#include <utility>

/**
 * @brief Indexed ring buffer with stable logical indexing and cheap head removal.
 *
 * The container presents a zero-based logical sequence while storing elements in a
 * circular backing array. It is intended for bounded output-history data where
 * `push_back()` plus `remove(0, n)` is common and must not move every retained
 * element. Iterators walk logical order and never expose physical storage order.
 *
 * @tparam T Stored value type. The type must be default-constructible because
 * released slots are reset to `T{}` so Qt implicitly-shared resources are freed.
 */
template <typename T> class IndexedRingBuffer
{
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

		template <typename Buffer, typename Reference, typename Pointer> class BasicIterator
		{
			public:
				using difference_type   = qsizetype;
				using value_type        = T;
				using reference         = Reference;
				using pointer           = Pointer;
				using iterator_concept  = std::random_access_iterator_tag;
				using iterator_category = std::random_access_iterator_tag;

				BasicIterator() = default;
				BasicIterator(Buffer *buffer, const size_type index) : m_buffer(buffer), m_index(index)
				{
				}

				reference operator*() const
				{
					return (*m_buffer)[m_index];
				}
				pointer operator->() const
				{
					return &(*m_buffer)[m_index];
				}

				BasicIterator &operator++()
				{
					++m_index;
					return *this;
				}
				BasicIterator operator++(int)
				{
					BasicIterator copy(*this);
					++(*this);
					return copy;
				}
				BasicIterator &operator--()
				{
					--m_index;
					return *this;
				}
				BasicIterator operator--(int)
				{
					BasicIterator copy(*this);
					--(*this);
					return copy;
				}
				BasicIterator &operator+=(const difference_type offset)
				{
					m_index += offset;
					return *this;
				}
				BasicIterator &operator-=(const difference_type offset)
				{
					m_index -= offset;
					return *this;
				}
				BasicIterator operator+(const difference_type offset) const
				{
					BasicIterator copy(*this);
					copy += offset;
					return copy;
				}
				friend BasicIterator operator+(const difference_type offset, const BasicIterator &iterator)
				{
					return iterator + offset;
				}
				BasicIterator operator-(const difference_type offset) const
				{
					BasicIterator copy(*this);
					copy -= offset;
					return copy;
				}
				difference_type operator-(const BasicIterator &other) const
				{
					return m_index - other.m_index;
				}
				reference operator[](const difference_type offset) const
				{
					return *(*this + offset);
				}

				bool operator==(const BasicIterator &other) const
				{
					return m_buffer == other.m_buffer && m_index == other.m_index;
				}
				bool operator!=(const BasicIterator &other) const
				{
					return !(*this == other);
				}
				bool operator<(const BasicIterator &other) const
				{
					return m_index < other.m_index;
				}
				bool operator>(const BasicIterator &other) const
				{
					return other < *this;
				}
				bool operator<=(const BasicIterator &other) const
				{
					return !(other < *this);
				}
				bool operator>=(const BasicIterator &other) const
				{
					return !(*this < other);
				}

				[[nodiscard]] size_type index() const
				{
					return m_index;
				}

			private:
				Buffer   *m_buffer{nullptr};
				size_type m_index{0};
		};

		using iterator       = BasicIterator<IndexedRingBuffer<T>, T &, T *>;
		using const_iterator = BasicIterator<const IndexedRingBuffer<T>, const T &, const T *>;

		IndexedRingBuffer() = default;
		explicit IndexedRingBuffer(const size_type count)
		{
			resize(count);
		}
		IndexedRingBuffer(const size_type count, const T &value)
		{
			assign(count, value);
		}
		explicit IndexedRingBuffer(const QVector<T> &values)
		{
			assign(values);
		}

		IndexedRingBuffer &operator=(const QVector<T> &values)
		{
			assign(values);
			return *this;
		}

		[[nodiscard]] size_type size() const
		{
			return m_size;
		}
		[[nodiscard]] bool isEmpty() const
		{
			return m_size == 0;
		}
		[[nodiscard]] size_type capacity() const
		{
			return m_storage.size();
		}

		void clear()
		{
			m_storage.clear();
			m_head = 0;
			m_size = 0;
		}

		void swap(IndexedRingBuffer &other) noexcept
		{
			m_storage.swap(other.m_storage);
			std::swap(m_head, other.m_head);
			std::swap(m_size, other.m_size);
		}

		void reserve(const size_type requestedCapacity)
		{
			if (requestedCapacity <= capacity())
				return;
			reallocate(qMax<size_type>(requestedCapacity, capacity() == 0 ? 8 : capacity() * 2));
		}

		void resize(const size_type newSize)
		{
			Q_ASSERT(newSize >= 0);
			if (newSize < m_size)
			{
				remove(newSize, m_size - newSize);
				return;
			}
			reserve(newSize);
			for (size_type index = m_size; index < newSize; ++index)
				storageAtLogical(index) = T{};
			m_size = newSize;
		}

		void truncateBack(const size_type newSize)
		    requires std::is_trivially_destructible_v<T>
		{
			Q_ASSERT(newSize >= 0 && newSize <= m_size);
			m_size = newSize;
			if (m_size == 0)
				m_head = 0;
		}

		void assign(const size_type count, const T &value)
		{
			clear();
			reserve(count);
			for (size_type index = 0; index < count; ++index)
				storageAtLogical(index) = value;
			m_size = count;
		}

		void assign(const QVector<T> &values)
		{
			clear();
			reserve(values.size());
			for (const T &value : values)
				push_back(value);
		}

		[[nodiscard]] T &operator[](const size_type index)
		{
			Q_ASSERT(index >= 0 && index < m_size);
			return storageAtLogical(index);
		}
		[[nodiscard]] const T &operator[](const size_type index) const
		{
			Q_ASSERT(index >= 0 && index < m_size);
			return storageAtLogical(index);
		}
		[[nodiscard]] T &at(const size_type index)
		{
			return (*this)[index];
		}
		[[nodiscard]] const T &at(const size_type index) const
		{
			return (*this)[index];
		}

		[[nodiscard]] T &first()
		{
			return (*this)[0];
		}
		[[nodiscard]] const T &first() const
		{
			return (*this)[0];
		}
		[[nodiscard]] const T &constFirst() const
		{
			return (*this)[0];
		}
		[[nodiscard]] T &last()
		{
			return (*this)[m_size - 1];
		}
		[[nodiscard]] const T &last() const
		{
			return (*this)[m_size - 1];
		}
		[[nodiscard]] const T &constLast() const
		{
			return (*this)[m_size - 1];
		}

		void push_back(const T &value)
		{
			reserve(m_size + 1);
			storageAtLogical(m_size) = value;
			++m_size;
		}
		void push_back(T &&value)
		{
			reserve(m_size + 1);
			storageAtLogical(m_size) = std::move(value);
			++m_size;
		}
		void append(const T &value)
		{
			push_back(value);
		}
		void append(T &&value)
		{
			push_back(std::move(value));
		}

		template <typename... Args> T &emplaceBack(Args &&...args)
		{
			reserve(m_size + 1);
			T &slot = storageAtLogical(m_size);
			slot    = T(std::forward<Args>(args)...);
			++m_size;
			return slot;
		}

		void pop_back()
		{
			Q_ASSERT(m_size > 0);
			storageAtLogical(m_size - 1) = T{};
			--m_size;
			if (m_size == 0)
				m_head = 0;
		}
		void removeLast()
		{
			pop_back();
		}

		void pop_front()
		{
			remove(0, 1);
		}
		void removeFirst()
		{
			pop_front();
		}

		void removeAt(const size_type index)
		{
			remove(index, 1);
		}

		void remove(const size_type index, const size_type count = 1)
		{
			if (count <= 0)
				return;
			Q_ASSERT(index >= 0 && index <= m_size);
			const size_type boundedCount = qMin(count, m_size - index);
			if (boundedCount <= 0)
				return;
			if (index == 0)
			{
				for (size_type i = 0; i < boundedCount; ++i)
					storageAtPhysical((m_head + i) % capacity()) = T{};
				m_head = (m_head + boundedCount) % capacity();
				m_size -= boundedCount;
				if (m_size == 0)
					m_head = 0;
				return;
			}
			if (index + boundedCount == m_size)
			{
				for (size_type i = index; i < m_size; ++i)
					storageAtLogical(i) = T{};
				m_size = index;
				return;
			}

			const size_type prefixCount = index;
			const size_type suffixCount = m_size - index - boundedCount;
			if (prefixCount < suffixCount)
			{
				for (size_type source = index; source > 0; --source)
					storageAtLogical(source + boundedCount - 1) = std::move(storageAtLogical(source - 1));
				for (size_type i = 0; i < boundedCount; ++i)
					storageAtPhysical((m_head + i) % capacity()) = T{};
				m_head = (m_head + boundedCount) % capacity();
			}
			else
			{
				for (size_type source = index + boundedCount; source < m_size; ++source)
					storageAtLogical(source - boundedCount) = std::move(storageAtLogical(source));
				for (size_type i = m_size - boundedCount; i < m_size; ++i)
					storageAtLogical(i) = T{};
			}
			m_size -= boundedCount;
		}

		/**
		 * @brief Replaces a logical range with repeated values in one structural mutation.
		 * @param replacement Logical range and replacement count.
		 * @param value Value copied into each inserted slot.
		 *
		 * This shifts either the retained prefix or retained suffix once, whichever is
		 * smaller, so middle range replacement does not degrade into repeated
		 * single-element insert/remove operations.
		 */
		void replace(const Replacement &replacement, const T &value)
		{
			const size_type index       = replacement.index;
			const size_type removeCount = replacement.removeCount;
			const size_type insertCount = replacement.insertCount;
			Q_ASSERT(index >= 0 && index <= m_size);
			Q_ASSERT(removeCount >= 0);
			Q_ASSERT(insertCount >= 0);

			const size_type boundedRemoveCount = qBound<size_type>(0, removeCount, m_size - index);
			const size_type boundedInsertCount = qMax<size_type>(0, insertCount);
			if (boundedRemoveCount == 0 && boundedInsertCount == 0)
				return;

			const size_type oldSize     = m_size;
			const size_type suffixFirst = index + boundedRemoveCount;
			const size_type suffixCount = oldSize - suffixFirst;
			const size_type newSize     = oldSize - boundedRemoveCount + boundedInsertCount;
			reserve(newSize);

			if (boundedInsertCount > boundedRemoveCount)
			{
				const size_type growth = boundedInsertCount - boundedRemoveCount;
				if (index < suffixCount)
				{
					const size_type oldHead = m_head;
					const size_type newHead = (m_head + capacity() - growth % capacity()) % capacity();
					for (size_type source = 0; source < index; ++source)
					{
						const size_type sourcePhysical         = (oldHead + source) % capacity();
						const size_type destinationPhysical    = (newHead + source) % capacity();
						storageAtPhysical(destinationPhysical) = std::move(storageAtPhysical(sourcePhysical));
					}
					m_head = newHead;
				}
				else
				{
					for (size_type offset = suffixCount; offset > 0; --offset)
					{
						const size_type source            = suffixFirst + offset - 1;
						storageAtLogical(source + growth) = std::move(storageAtLogical(source));
					}
				}
			}
			else if (boundedRemoveCount > boundedInsertCount)
			{
				const size_type shrink = boundedRemoveCount - boundedInsertCount;
				if (index < suffixCount)
				{
					const size_type oldHead = m_head;
					for (size_type source = index; source > 0; --source)
					{
						const size_type sourceLogical       = source - 1;
						const size_type sourcePhysical      = (oldHead + sourceLogical) % capacity();
						const size_type destinationPhysical = (oldHead + sourceLogical + shrink) % capacity();
						storageAtPhysical(destinationPhysical) = std::move(storageAtPhysical(sourcePhysical));
					}
					for (size_type i = 0; i < shrink; ++i)
						storageAtPhysical((oldHead + i) % capacity()) = T{};
					m_head = (oldHead + shrink) % capacity();
				}
				else
				{
					for (size_type offset = 0; offset < suffixCount; ++offset)
					{
						const size_type source            = suffixFirst + offset;
						storageAtLogical(source - shrink) = std::move(storageAtLogical(source));
					}
					for (size_type i = newSize; i < oldSize; ++i)
						storageAtLogical(i) = T{};
				}
			}

			m_size = newSize;
			if (m_size == 0)
			{
				m_head = 0;
				return;
			}
			for (size_type i = 0; i < boundedInsertCount; ++i)
				storageAtLogical(index + i) = value;
		}

		void insert(const size_type index, const T &value)
		{
			insertImpl(index, value);
		}
		void insert(const size_type index, T &&value)
		{
			insertImpl(index, std::move(value));
		}
		iterator insert(const_iterator position, const T &value)
		{
			insert(position.index(), value);
			return iterator(this, position.index());
		}
		iterator insert(const_iterator position, T &&value)
		{
			insert(position.index(), std::move(value));
			return iterator(this, position.index());
		}
		iterator insert(iterator position, const T &value)
		{
			insert(position.index(), value);
			return iterator(this, position.index());
		}
		iterator insert(iterator position, T &&value)
		{
			insert(position.index(), std::move(value));
			return iterator(this, position.index());
		}

		iterator erase(const_iterator first, const_iterator last)
		{
			const size_type index = first.index();
			remove(index, last.index() - first.index());
			return iterator(this, index);
		}
		iterator erase(const_iterator position)
		{
			return erase(position, position + 1);
		}
		iterator erase(iterator first, iterator last)
		{
			const size_type index = first.index();
			remove(index, last.index() - first.index());
			return iterator(this, index);
		}
		iterator erase(iterator position)
		{
			return erase(position, position + 1);
		}

		[[nodiscard]] QVector<T> toVector() const
		{
			QVector<T> result;
			result.reserve(m_size);
			for (size_type index = 0; index < m_size; ++index)
				result.push_back(at(index));
			return result;
		}

		/**
		 * @brief Finds the first logical element accepted by an indexed predicate.
		 * @tparam Predicate Callable type accepting a stored value and its logical index.
		 * @param predicate Callable receiving `(const T &, logicalIndex)` and returning whether to stop.
		 * @return Zero-based logical index of the first accepted element, or `-1` when none is accepted.
		 *
		 * The circular storage is traversed as at most two contiguous physical ranges, avoiding a
		 * modulo operation for every logical element while preserving logical-order semantics.
		 */
		template <typename Predicate> [[nodiscard]] size_type findIndexIf(Predicate &&predicate) const
		{
			if (m_size <= 0)
				return -1;

			const T *const  storage               = m_storage.constData();
			const size_type firstPhysicalRunCount = qMin(m_size, capacity() - m_head);
			for (size_type logicalIndex = 0; logicalIndex < firstPhysicalRunCount; ++logicalIndex)
			{
				if (predicate(storage[m_head + logicalIndex], logicalIndex))
					return logicalIndex;
			}
			for (size_type logicalIndex = firstPhysicalRunCount; logicalIndex < m_size; ++logicalIndex)
			{
				if (predicate(storage[logicalIndex - firstPhysicalRunCount], logicalIndex))
					return logicalIndex;
			}
			return -1;
		}

		[[nodiscard]] iterator begin()
		{
			return iterator(this, 0);
		}
		[[nodiscard]] iterator end()
		{
			return iterator(this, m_size);
		}
		[[nodiscard]] const_iterator begin() const
		{
			return const_iterator(this, 0);
		}
		[[nodiscard]] const_iterator end() const
		{
			return const_iterator(this, m_size);
		}
		[[nodiscard]] const_iterator cbegin() const
		{
			return const_iterator(this, 0);
		}
		[[nodiscard]] const_iterator cend() const
		{
			return const_iterator(this, m_size);
		}

	private:
		template <typename Value> void insertImpl(const size_type index, Value &&value)
		{
			Q_ASSERT(index >= 0 && index <= m_size);
			if (index == m_size)
			{
				push_back(std::forward<Value>(value));
				return;
			}

			reserve(m_size + 1);
			if (index < m_size - index)
			{
				const size_type oldHead = m_head;
				const size_type newHead = (m_head + capacity() - 1) % capacity();
				for (size_type source = 0; source < index; ++source)
				{
					const size_type destinationPhysical    = (newHead + source) % capacity();
					const size_type sourcePhysical         = (oldHead + source) % capacity();
					storageAtPhysical(destinationPhysical) = std::move(storageAtPhysical(sourcePhysical));
				}
				m_head = newHead;
			}
			else
			{
				for (size_type target = m_size; target > index; --target)
					storageAtLogical(target) = std::move(storageAtLogical(target - 1));
			}
			storageAtLogical(index) = std::forward<Value>(value);
			++m_size;
		}

		void reallocate(const size_type newCapacity)
		{
			QVector<T> compact(newCapacity);
			for (size_type index = 0; index < m_size; ++index)
				compact[index] = std::move(storageAtLogical(index));
			m_storage = std::move(compact);
			m_head    = 0;
		}

		[[nodiscard]] size_type physicalIndex(const size_type logicalIndex) const
		{
			Q_ASSERT(!m_storage.isEmpty());
			return (m_head + logicalIndex) % m_storage.size();
		}
		[[nodiscard]] T &storageAtLogical(const size_type logicalIndex)
		{
			return m_storage[physicalIndex(logicalIndex)];
		}
		[[nodiscard]] const T &storageAtLogical(const size_type logicalIndex) const
		{
			return m_storage[physicalIndex(logicalIndex)];
		}
		[[nodiscard]] T &storageAtPhysical(const size_type physicalIndex)
		{
			return m_storage[physicalIndex];
		}

		QVector<T> m_storage;
		size_type  m_head{0};
		size_type  m_size{0};
};
