/**
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * @file ProjectedIndexedRingBuffer.h
 * @brief Synchronous sparse render projections over an unchanged indexed sequence.
 */

#pragma once

#include "IndexedRingBuffer.h"
#include <memory>
#include <vector>

/**
 * @brief Indexed sequence with a synchronous projection that owns only changed ranges.
 *
 * Ordinary storage delegates to IndexedRingBuffer. A projection borrows its source until destruction;
 * it must never outlive or mutate that source, invoke callbacks, or cross an asynchronous boundary.
 * Replacements split borrowed ranges and own the changed rows, avoiding a full backing-array detach.
 * @tparam T Stored row type.
 * @tparam Metadata Static access to the row index and its uniform adjustment.
 */
template <typename T, typename Metadata> class ProjectedIndexedRingBuffer
{
	public:
		using size_type  = qsizetype;
		using value_type = T;
		using iterator   = IndexedRingBuffer<T>::template BasicIterator<ProjectedIndexedRingBuffer, T &, T *>;
		using const_iterator = IndexedRingBuffer<T>::template BasicIterator<const ProjectedIndexedRingBuffer,
		                                                                    const T &, const T *>;

		/** @brief Constructs an ordinary empty sequence. */
		ProjectedIndexedRingBuffer() = default;

		/**
		 * @brief Borrows this sequence for a synchronous projection.
		 * @return A projection whose source must remain alive and unchanged.
		 */
		[[nodiscard]] ProjectedIndexedRingBuffer project() const
		{
			if (m_source)
				return *this;
			ProjectedIndexedRingBuffer result;
			result.m_source = this;
			result.m_size   = size();
			if (result.m_size)
				result.m_pieces.push_back({0, result.m_size, {}});
			return result;
		}
		/** @brief Returns the logical row count. @return Row count. */
		[[nodiscard]] size_type size() const
		{
			return m_source ? m_size : m_storage.size();
		}
		/** @brief Checks for an empty sequence. @return Whether no rows exist. */
		[[nodiscard]] bool isEmpty() const
		{
			return size() == 0;
		}
		/**
		 * @brief Reads a logical row without copying it.
		 * @param index Logical index.
		 * @return Borrowed or owned row.
		 */
		[[nodiscard]] const T &operator[](size_type index) const
		{
			if (!m_source)
				return m_storage[index];
			Q_ASSERT(index >= 0 && index < m_size);
			for (const Piece &piece : m_pieces)
			{
				if (index < piece.count)
					return piece.owned ? std::as_const(*piece.owned)[piece.start + index]
					                   : std::as_const(*m_source)[piece.start + index];
				index -= piece.count;
			}
			Q_UNREACHABLE();
		}
		/**
		 * @brief Obtains a mutable row, owning that row if it was borrowed.
		 * @param index Logical index.
		 * @return Mutable row local to this sequence.
		 */
		[[nodiscard]] T &operator[](size_type index)
		{
			if (!m_source)
				return m_storage[index];
			Q_ASSERT(index >= 0 && index < m_size);
			size_type position = 0;
			for (std::size_t i = 0; i < m_pieces.size(); ++i)
			{
				const Piece &candidate = m_pieces[i];
				if (index < position + candidate.count)
				{
					const size_type offset = index - position;
					if (candidate.owned && candidate.owned.unique() && candidate.indexAdjustment == 0)
					{
						auto &storage = m_pieces[i].owned;
						return (*storage)[candidate.start + offset];
					}
					const Piece piece   = candidate;
					auto        storage = std::make_shared<IndexedRingBuffer<T>>();
					storage->push_back(piece.owned ? std::as_const(*piece.owned)[piece.start + offset]
					                               : std::as_const(*m_source)[piece.start + offset]);
					Metadata::adjust((*storage)[0], piece.indexAdjustment);
					std::vector<Piece> replacement;
					if (offset)
						replacement.push_back({piece.start, offset, piece.owned, piece.indexAdjustment});
					replacement.push_back({0, 1, storage});
					if (offset + 1 < piece.count)
						replacement.push_back({piece.start + offset + 1, piece.count - offset - 1,
						                       piece.owned, piece.indexAdjustment});
					m_pieces.erase(m_pieces.begin() + static_cast<std::ptrdiff_t>(i));
					m_pieces.insert(m_pieces.begin() + static_cast<std::ptrdiff_t>(i), replacement.begin(),
					                replacement.end());
					return (*storage)[0];
				}
				position += candidate.count;
			}
			Q_UNREACHABLE();
		}
		/** @brief Reads a row. @param index Logical index. @return Row at the index. */
		[[nodiscard]] const T &at(size_type index) const
		{
			return (*this)[index];
		}
		/** @brief Obtains a mutable row. @param index Logical index. @return Mutable row. */
		[[nodiscard]] T &at(size_type index)
		{
			return (*this)[index];
		}
		/** @brief Reads the first row. @return First row. */
		[[nodiscard]] const T &first() const
		{
			return (*this)[0];
		}
		/** @brief Reads the first row. @return First row. */
		[[nodiscard]] const T &constFirst() const
		{
			return first();
		}
		/** @brief Obtains the first row. @return Mutable first row. */
		[[nodiscard]] T &first()
		{
			return (*this)[0];
		}
		/** @brief Reads the last row. @return Last row. */
		[[nodiscard]] const T &last() const
		{
			return (*this)[size() - 1];
		}
		/** @brief Obtains the last row. @return Mutable last row. */
		[[nodiscard]] T &last()
		{
			return (*this)[size() - 1];
		}
		/** @brief Reads the last row. @return Last row. */
		[[nodiscard]] const T &constLast() const
		{
			return last();
		}
		/** @brief Reserves ordinary storage; projections allocate only changed rows. @param count Requested capacity. */
		void reserve(size_type count)
		{
			if (!m_source)
				m_storage.reserve(count);
		}
		/** @brief Removes all rows without modifying borrowed source storage. */
		void clear()
		{
			if (m_source)
			{
				m_pieces.clear();
				m_size = 0;
			}
			else
				m_storage.clear();
		}
		/** @brief Appends an owned row. @param value Row to append. */
		void push_back(T value)
		{
			if (!m_source)
			{
				m_storage.push_back(std::move(value));
				return;
			}
			if (!m_pieces.empty())
			{
				Piece &tail = m_pieces.back();
				if (tail.owned && tail.owned.unique() && tail.indexAdjustment == 0 &&
				    tail.start + tail.count == tail.owned->size())
				{
					tail.owned->push_back(std::move(value));
					++tail.count;
					++m_size;
					return;
				}
			}
			auto storage = std::make_shared<IndexedRingBuffer<T>>();
			storage->push_back(std::move(value));
			m_pieces.push_back({0, 1, std::move(storage)});
			++m_size;
		}
		/**
		 * @brief Removes a logical range without copying the retained rows.
		 * @param index First row to remove.
		 * @param count Number of rows to remove.
		 */
		void remove(size_type index, size_type count)
		{
			if (!m_source)
			{
				m_storage.remove(index, count);
				return;
			}
			Q_ASSERT(index >= 0 && count >= 0 && index + count <= m_size);
			if (!count)
				return;
			std::vector<Piece> kept;
			size_type          position = 0;
			for (const Piece &piece : m_pieces)
			{
				const size_type left  = qBound<size_type>(0, index - position, piece.count);
				const size_type right = qBound<size_type>(0, index + count - position, piece.count);
				if (left)
					kept.push_back({piece.start, left, piece.owned, piece.indexAdjustment});
				if (right < piece.count)
					kept.push_back(
					    {piece.start + right, piece.count - right, piece.owned, piece.indexAdjustment});
				position += piece.count;
			}
			m_pieces = std::move(kept);
			m_size -= count;
		}
		/**
		 * @brief Reads an effective row index without materializing a borrowed row.
		 * @param index Logical row index.
		 * @return Adjusted metadata index, retaining a negative sentinel.
		 */
		[[nodiscard]] qint64 effectiveIndex(size_type index) const
		{
			if (!m_source)
				return Metadata::index(m_storage[index]);
			Q_ASSERT(index >= 0 && index < m_size);
			for (const Piece &piece : m_pieces)
			{
				if (index < piece.count)
				{
					const T     &row   = piece.owned ? std::as_const(*piece.owned)[piece.start + index]
					                                 : std::as_const(*m_source)[piece.start + index];
					const qint64 value = Metadata::index(row);
					return value < 0 ? value : value + piece.indexAdjustment;
				}
				index -= piece.count;
			}
			Q_UNREACHABLE();
		}
		/**
		 * @brief Extracts a suffix, retaining projection ranges against the original source.
		 * @param index First row in the extracted suffix.
		 * @return Removed suffix with the same storage and borrowing mode.
		 */
		[[nodiscard]] ProjectedIndexedRingBuffer takeSuffix(size_type index)
		{
			Q_ASSERT(index >= 0 && index <= size());
			ProjectedIndexedRingBuffer suffix;
			if (!m_source)
			{
				suffix.reserve(size() - index);
				for (size_type i = index; i < size(); ++i)
					suffix.push_back(std::move(m_storage[i]));
				m_storage.remove(index, size() - index);
				return suffix;
			}
			suffix.m_source             = m_source;
			suffix.m_size               = m_size - index;
			size_type          position = 0;
			std::vector<Piece> prefix;
			for (const Piece &piece : m_pieces)
			{
				const size_type before = qBound<size_type>(0, index - position, piece.count);
				if (before)
					prefix.push_back({piece.start, before, piece.owned, piece.indexAdjustment});
				if (before < piece.count)
					suffix.appendPiece(
					    {piece.start + before, piece.count - before, piece.owned, piece.indexAdjustment});
				position += piece.count;
			}
			m_pieces = std::move(prefix);
			m_size   = index;
			return suffix;
		}
		/**
		 * @brief Rejoins an extracted suffix with a uniform metadata index adjustment.
		 * @param suffix Suffix extracted from the same source and storage mode; emptied on return.
		 * @param indexAdjustment Additional adjustment in runtime entries, independent of rendered row count.
		 */
		void appendSuffix(ProjectedIndexedRingBuffer &&suffix, qint64 indexAdjustment)
		{
			Q_ASSERT(this != &suffix && m_source == suffix.m_source);
			if (!m_source)
			{
				for (T &row : suffix.m_storage)
				{
					Metadata::adjust(row, indexAdjustment);
					m_storage.push_back(std::move(row));
				}
			}
			else
			{
				m_size += suffix.m_size;
				for (Piece &piece : suffix.m_pieces)
				{
					piece.indexAdjustment += indexAdjustment;
					appendPiece(std::move(piece));
				}
			}
			suffix.clear();
		}
		/** @brief Removes the last logical row. */
		void removeLast()
		{
			remove(size() - 1, 1);
		}
		/** @brief Returns a mutable begin iterator. @return Begin iterator. */
		[[nodiscard]] iterator begin()
		{
			return {this, 0};
		}
		/** @brief Returns a mutable end iterator. @return End iterator. */
		[[nodiscard]] iterator end()
		{
			return {this, size()};
		}
		/** @brief Returns a read-only begin iterator. @return Begin iterator. */
		[[nodiscard]] const_iterator begin() const
		{
			return {this, 0};
		}
		/** @brief Returns a read-only end iterator. @return End iterator. */
		[[nodiscard]] const_iterator end() const
		{
			return {this, size()};
		}
		/** @brief Returns a read-only begin iterator. @return Begin iterator. */
		[[nodiscard]] const_iterator cbegin() const
		{
			return begin();
		}
		/** @brief Returns a read-only end iterator. @return End iterator. */
		[[nodiscard]] const_iterator cend() const
		{
			return end();
		}

	private:
		/** @brief Owned or borrowed contiguous range within a synchronous projection. */
		struct Piece
		{
				size_type                             start{0};
				size_type                             count{0};
				std::shared_ptr<IndexedRingBuffer<T>> owned;
				qint64                                indexAdjustment{0};
		};
		/**
		 * @brief Appends a range descriptor and joins compatible adjacent ranges.
		 * @param piece Nonempty owned or borrowed range.
		 */
		void appendPiece(Piece piece)
		{
			if (piece.count == 0)
				return;
			if (!m_pieces.empty())
			{
				auto &tail = m_pieces.back();
				if (tail.owned == piece.owned && tail.indexAdjustment == piece.indexAdjustment &&
				    tail.start + tail.count == piece.start)
				{
					tail.count += piece.count;
					return;
				}
			}
			m_pieces.push_back(std::move(piece));
		}
		IndexedRingBuffer<T>              m_storage;
		const ProjectedIndexedRingBuffer *m_source{nullptr};
		std::vector<Piece>                m_pieces;
		size_type                         m_size{0};
};
