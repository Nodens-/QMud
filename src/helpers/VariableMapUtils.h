/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: VariableMapUtils.h
 * Role: Variable-name boundary normalization, canonical storage, and callback-local variable views.
 */

#ifndef QMUD_VARIABLEMAPUTILS_H
#define QMUD_VARIABLEMAPUTILS_H

#include <QHash>
#include <QMap>
#include <QString>

#include <optional>
#include <utility>

namespace QMudVariableMapUtils
{
	/** @brief Variable name normalized once when external input enters the variable APIs. */
	class Name
	{
		public:
			/**
			 * @brief Normalizes an external lookup key by trimming and lowercasing it.
			 * @param input External variable name; its original spelling is retained for serialization.
			 */
			Name(const QString &input) : m_key(normalizeKey(input)), m_label(input)
			{
			}
			/**
			 * @brief Returns the canonical lookup key without further normalization.
			 * @return Canonical variable key.
			 */
			[[nodiscard]] const QString &key() const
			{
				return m_key;
			}
			/**
			 * @brief Returns the original external spelling.
			 * @return Variable label for serialization.
			 */
			[[nodiscard]] const QString &label() const
			{
				return m_label;
			}

		private:
			/**
			 * @brief Normalizes ASCII names without Unicode conversion, retaining Qt's Unicode fallback.
			 * @param input External name with optional surrounding whitespace.
			 * @return Trimmed, lowercase canonical key with the existing Unicode semantics.
			 */
			static QString normalizeKey(const QString &input)
			{
				QString      key        = input.trimmed();
				const QChar *data       = key.constData();
				qsizetype    firstUpper = -1;
				for (qsizetype index = 0; index < key.size(); ++index)
				{
					const ushort character = data[index].unicode();
					if (character > 0x7f)
						return std::move(key).toLower();
					if (firstUpper < 0 && character >= 'A' && character <= 'Z')
						firstUpper = index;
				}
				if (firstUpper >= 0)
				{
					QChar *writable = key.data();
					for (qsizetype index = firstUpper; index < key.size(); ++index)
					{
						const ushort character = writable[index].unicode();
						if (character >= 'A' && character <= 'Z')
							writable[index] = QChar{static_cast<ushort>(character + ('a' - 'A'))};
					}
				}
				return key;
			}

			QString m_key;
			QString m_label;
	};

	/** @brief Owning variable value and original XML metadata, independent of any runtime or VM. */
	struct Record
	{
			QMap<QString, QString> attributes;
			QString                content;
			QString                key;
			QString                label;
			/** @brief Creates an empty variable record. */
			Record() = default;
			/**
			 * @brief Creates a value-only snapshot record.
			 * @param value Variable content.
			 */
			Record(QString value) : content(std::move(value))
			{
			}
			/**
			 * @brief Creates a record from parsed XML metadata.
			 * @param metadata Original XML attributes.
			 * @param value Variable content.
			 * @param canonicalKey Already normalized identity when available.
			 */
			Record(QMap<QString, QString> metadata, QString value, QString canonicalKey = {})
			    : attributes(std::move(metadata)), content(std::move(value)), key(std::move(canonicalKey)),
			      label(attributes.value(QStringLiteral("name")))
			{
			}
			/**
			 * @brief Compares variable content and metadata without depending on iteration order.
			 * @param other Record to compare.
			 * @return Whether the records are identical.
			 */
			bool operator==(const Record &other) const = default;
	};

	using VariableMap = QHash<QString, Record>;

	/**
	 * @brief Inserts or assigns one variable, avoiding detachment of an unchanged shared hash.
	 * @param variables Owning hash to update.
	 * @param key Already canonical identity.
	 * @param label Original spelling retained unchanged.
	 * @param value New content.
	 * @param retainXmlAttributes Keep the name in world XML metadata.
	 * @return Whether writable access was needed.
	 */
	inline bool assignCanonical(VariableMap &variables, const QString &key, const QString &label,
	                            const QString &value, bool retainXmlAttributes = false)
	{
		if (key.isEmpty())
			return false;
		if (!variables.isDetached())
		{
			const auto current = variables.constFind(key);
			if (current != variables.cend() && current->content == value && current->label == label)
				return false;
		}
		auto [it, inserted] = variables.tryEmplace(key);
		Q_UNUSED(inserted);
		it->content = value;
		it->label   = label;
		it->key     = key;
		if (retainXmlAttributes || !it->attributes.isEmpty())
			it->attributes.insert(QStringLiteral("name"), label);
		return true;
	}

	/**
	 * @brief Assigns a boundary-normalized variable without renormalizing internal keys.
	 * @param variables Owning hash to update.
	 * @param name Boundary-normalized identity and original spelling.
	 * @param value New content.
	 * @param retainXmlAttributes Keep the name in world XML metadata.
	 * @return Whether writable access was needed.
	 */
	inline bool assign(VariableMap &variables, const Name &name, const QString &value,
	                   bool retainXmlAttributes = false)
	{
		return assignCanonical(variables, name.key(), name.label(), value, retainXmlAttributes);
	}

	/** @brief Canonical variable records shared by runtime, callback and persistence owners. */
	class Store
	{
		public:
			/** @brief Returns immutable owning records. @return Canonical variable hash. */
			[[nodiscard]] const VariableMap &values() const
			{
				return m_values;
			}
			/**
			 * @brief Reads a stored original spelling.
			 * @param key Canonical identity.
			 * @return Original spelling, or empty when absent.
			 */
			[[nodiscard]] QString label(const QString &key) const
			{
				const auto it = m_values.constFind(key);
				return it == m_values.cend() ? QString() : it->label;
			}
			/**
			 * @brief Reads stored content.
			 * @param key Canonical identity.
			 * @return Content, or empty when absent.
			 */
			[[nodiscard]] QString value(const QString &key) const
			{
				const auto it = m_values.constFind(key);
				return it == m_values.cend() ? QString() : it->content;
			}
			/** @brief Returns the number of named variables. @return Variable count. */
			[[nodiscard]] qsizetype size() const
			{
				return m_values.size();
			}
			/**
			 * @brief Visits each owning name/value pair once in unspecified order.
			 * @param visitor Callable accepting original spelling and content; it must not mutate the store.
			 */
			template <typename Visitor> void forEachVariable(Visitor &&visitor) const
			{
				for (const Record &record : m_values)
					visitor(record.label, record.content);
			}
			/**
			 * @brief Assigns content and spelling with one variable hash.
			 * @param name Boundary-normalized identity and spelling.
			 * @param value New content.
			 * @return Whether writable access was needed.
			 */
			bool insert(const Name &name, const QString &value)
			{
				return assign(m_values, name, value);
			}
			/**
			 * @brief Removes a variable using the container's removal result.
			 * @param key Canonical identity.
			 * @return Whether an entry was removed.
			 */
			bool remove(const QString &key)
			{
				return m_values.remove(key);
			}
			/** @brief Clears the store. */
			void clear()
			{
				m_values.clear();
			}

		private:
			VariableMap m_values;
	};

	/**
	 * @brief Resolves content and presence with one hash lookup.
	 * @param variables Immutable records.
	 * @param name Canonical identity.
	 * @param value Receives existing content, including an empty string.
	 * @return Whether the variable exists.
	 */
	[[nodiscard]] inline bool findValue(const VariableMap &variables, const QString &name, QString &value)
	{
		const auto it = variables.constFind(name);
		if (it == variables.cend())
			return false;
		value = it->content;
		return true;
	}

	/** @brief Lane-local complete hash or sparse knowledge for an omitted variable owner. */
	class WorkingView
	{
		public:
			/** @brief Whether the complete owner map was captured or is unavailable. */
			enum class Source
			{
				Uncaptured,
				Available,
				Unavailable
			};
			/** @brief Returns source knowledge. @return Current source state. */
			[[nodiscard]] Source source() const
			{
				return m_source;
			}
			/**
			 * @brief Installs an owning complete view and applies explicit sparse edits once.
			 * @param values Captured canonical records.
			 */
			void capture(const VariableMap &values)
			{
				m_current = values;
				for (auto it = m_sparse.cbegin(); it != m_sparse.cend(); ++it)
					if (it->written)
						applySparse(m_current, it.key(), *it);
				m_sparse.clear();
				m_source = Source::Available;
			}
			/** @brief Discards owner knowledge and writes after topology invalidation. */
			void makeUnavailable()
			{
				*this    = WorkingView{};
				m_source = Source::Unavailable;
			}
			/**
			 * @brief Reads a complete hash directly, or resolves explicit sparse knowledge.
			 * @param name Canonical identity.
			 * @param value Receives existing content.
			 * @param resolved Receives whether presence is known.
			 * @return Whether the variable exists.
			 */
			bool find(const QString &name, QString &value, bool &resolved) const
			{
				resolved = m_source != Source::Uncaptured;
				if (m_source == Source::Unavailable)
					return false;
				if (m_source == Source::Available)
					return findValue(m_current, name, value);
				const auto it = m_sparse.constFind(name);
				if (it == m_sparse.cend())
					return false;
				resolved = true;
				if (it->value)
					value = it->value->content;
				return it->value.has_value();
			}
			/**
			 * @brief Remembers fallback observations only while a complete map is absent.
			 * @param name Canonical identity.
			 * @param present Whether the value exists.
			 * @param value Observed content.
			 */
			void remember(const QString &name, bool present, const QString &value)
			{
				if (m_source != Source::Uncaptured)
					return;
				auto &entry = m_sparse[name];
				if (!entry.written)
					entry.value = present ? std::optional<Record>(Record(value)) : std::nullopt;
			}
			/**
			 * @brief Updates local visibility without changing executable journal ownership.
			 * @param name Boundary-normalized identity and spelling.
			 * @param value Assigned content.
			 * @param remove Whether to delete the value.
			 * @param retainXmlAttributes Preserve original world XML metadata.
			 */
			void write(const Name &name, const QString &value, bool remove, bool retainXmlAttributes = false)
			{
				if (m_source == Source::Unavailable)
					return;
				m_hasWrites = true;
				if (m_source == Source::Available)
				{
					if (remove)
						m_current.remove(name.key());
					else
						assign(m_current, name, value, retainXmlAttributes);
					return;
				}
				auto &entry   = m_sparse[name.key()];
				entry.written = true;
				if (remove)
					entry.value.reset();
				else
				{
					Record record(value);
					record.key   = name.key();
					record.label = name.label();
					if (retainXmlAttributes)
						record.attributes.insert(QStringLiteral("name"), name.label());
					entry.value = std::move(record);
				}
			}
			/** @brief Returns a count only for a complete owner. @return Count, or no value. */
			[[nodiscard]] std::optional<qsizetype> count() const
			{
				return m_source == Source::Available ? std::optional<qsizetype>(m_current.size())
				                                     : std::nullopt;
			}
			/** @brief Reports explicit semantic writes. @return Whether a write was accepted. */
			[[nodiscard]] bool hasWrites() const
			{
				return m_hasWrites;
			}
			/** @brief Borrows complete immutable records. @return Complete hash, or null when omitted/unavailable. */
			[[nodiscard]] const VariableMap *values() const
			{
				return m_source == Source::Available ? &m_current : nullptr;
			}
			/**
			 * @brief Shares complete records or projects explicit sparse edits without publishing reads.
			 * @param target Owning target hash.
			 */
			void project(VariableMap &target) const
			{
				if (!m_hasWrites)
					return;
				if (m_source == Source::Available)
					target = m_current;
				else if (m_source == Source::Uncaptured)
					for (auto it = m_sparse.cbegin(); it != m_sparse.cend(); ++it)
						if (it->written)
							applySparse(target, it.key(), *it);
			}

		private:
			/** @brief Sparse observation or explicit mutation while the full owner is omitted. */
			struct SparseEntry
			{
					std::optional<Record> value;
					bool                  written{false};
			};
			/**
			 * @brief Applies one sparse mutation to an owning hash.
			 * @param target Hash receiving the edit.
			 * @param name Canonical identity.
			 * @param entry Explicit sparse edit.
			 */
			static void applySparse(VariableMap &target, const QString &name, const SparseEntry &entry)
			{
				if (entry.value)
					assignCanonical(target, name, entry.value->label, entry.value->content,
					                !entry.value->attributes.isEmpty());
				else
					target.remove(name);
			}
			Source                      m_source{Source::Uncaptured};
			VariableMap                 m_current;
			QHash<QString, SparseEntry> m_sparse;
			bool                        m_hasWrites{false};
	};
} // namespace QMudVariableMapUtils
#endif // QMUD_VARIABLEMAPUTILS_H
