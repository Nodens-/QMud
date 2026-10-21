/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SqliteCompat.cpp
 * Role: SQLite compatibility behavior shared by native and Lua-facing database operations.
 */

#include "SqliteCompat.h"

#include <array>

namespace
{
	enum class SqliteCompleteToken : unsigned char
	{
		Semicolon,
		Whitespace,
		Other,
		Explain,
		Create,
		Temporary,
		Trigger,
		End
	};

	/**
	 * @brief Reports whether a UTF-16 code unit can continue an unquoted SQLite identifier.
	 * @param codeUnit Code unit to classify.
	 * @return True when the code unit belongs to an SQLite identifier.
	 */
	bool isSqliteIdentifierCodeUnit(const QChar codeUnit)
	{
		const ushort value = codeUnit.unicode();
		return value >= 0x80 || (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
		       (value >= '0' && value <= '9') || value == '_' || value == '$';
	}

	/**
	 * @brief Compares an identifier slice to a lowercase ASCII SQLite keyword.
	 * @param sql SQL text containing the identifier.
	 * @param offset First identifier code unit.
	 * @param length Identifier length.
	 * @param keyword Null-terminated lowercase ASCII keyword.
	 * @return True when the identifier matches the keyword without regard to ASCII case.
	 */
	bool sqliteKeywordEquals(const QStringView sql, const qsizetype offset, const qsizetype length,
	                         const char *keyword)
	{
		for (qsizetype index = 0; index < length; ++index)
		{
			ushort value = sql.at(offset + index).unicode();
			if (value >= 'A' && value <= 'Z')
				value = static_cast<ushort>(value - 'A' + 'a');
			if (value != static_cast<unsigned char>(keyword[index]))
				return false;
		}
		return keyword[length] == '\0';
	}

	/**
	 * @brief Classifies identifiers used by SQLite's completeness state machine.
	 * @param sql SQL text containing the identifier.
	 * @param offset First identifier code unit.
	 * @param length Identifier length.
	 * @return Completeness token for the identifier.
	 */
	SqliteCompleteToken sqliteKeywordToken(const QStringView sql, const qsizetype offset,
	                                       const qsizetype length)
	{
		if (length == 3 && sqliteKeywordEquals(sql, offset, length, "end"))
			return SqliteCompleteToken::End;
		if (length == 4 && sqliteKeywordEquals(sql, offset, length, "temp"))
			return SqliteCompleteToken::Temporary;
		if (length == 6 && sqliteKeywordEquals(sql, offset, length, "create"))
			return SqliteCompleteToken::Create;
		if (length == 7 && sqliteKeywordEquals(sql, offset, length, "explain"))
			return SqliteCompleteToken::Explain;
		if (length == 7 && sqliteKeywordEquals(sql, offset, length, "trigger"))
			return SqliteCompleteToken::Trigger;
		if (length == 9 && sqliteKeywordEquals(sql, offset, length, "temporary"))
			return SqliteCompleteToken::Temporary;
		return SqliteCompleteToken::Other;
	}
} // namespace

SqliteStatementScanner::SqliteStatementScanner(const QStringView sql) : m_sql(sql)
{
	const qsizetype nul = m_sql.indexOf(QChar::Null);
	m_sqlSize           = nul < 0 ? m_sql.size() : nul;
}

bool SqliteStatementScanner::next(QStringView &statement)
{
	static constexpr std::array<std::array<State, 8>, 8> transitions{
	    {{State::Start, State::Invalid, State::Normal, State::Explain, State::Create, State::Normal,
	      State::Normal, State::Normal},
	     {State::Start, State::Start, State::Normal, State::Explain, State::Create, State::Normal,
	      State::Normal, State::Normal},
	     {State::Start, State::Normal, State::Normal, State::Normal, State::Normal, State::Normal,
	      State::Normal, State::Normal},
	     {State::Start, State::Explain, State::Explain, State::Normal, State::Create, State::Normal,
	      State::Normal, State::Normal},
	     {State::Start, State::Create, State::Normal, State::Normal, State::Normal, State::Create,
	      State::Trigger, State::Normal},
	     {State::Semicolon, State::Trigger, State::Trigger, State::Trigger, State::Trigger, State::Trigger,
	      State::Trigger, State::Trigger},
	     {State::Semicolon, State::Semicolon, State::Trigger, State::Trigger, State::Trigger, State::Trigger,
	      State::Trigger, State::End},
	     {State::Start, State::End, State::Trigger, State::Trigger, State::Trigger, State::Trigger,
	      State::Trigger, State::Trigger}}
    };

	if (m_exhausted)
		return false;

	for (; m_offset < m_sqlSize; ++m_offset)
	{
		SqliteCompleteToken token = SqliteCompleteToken::Other;
		switch (m_sql.at(m_offset).unicode())
		{
		case ';':
			token = SqliteCompleteToken::Semicolon;
			break;
		case ' ':
		case '\r':
		case '\t':
		case '\n':
		case '\f':
			token = SqliteCompleteToken::Whitespace;
			break;
		case '/':
			if (m_offset + 1 < m_sqlSize && m_sql.at(m_offset + 1) == QLatin1Char('*'))
			{
				m_offset += 2;
				while (m_offset < m_sqlSize &&
				       (m_sql.at(m_offset) != QLatin1Char('*') || m_offset + 1 >= m_sqlSize ||
				        m_sql.at(m_offset + 1) != QLatin1Char('/')))
					++m_offset;
				if (m_offset < m_sqlSize)
				{
					++m_offset;
					token = SqliteCompleteToken::Whitespace;
				}
				else
				{
					m_lexicallyComplete = false;
					token               = SqliteCompleteToken::Other;
				}
			}
			break;
		case '-':
			if (m_offset + 1 < m_sqlSize && m_sql.at(m_offset + 1) == QLatin1Char('-'))
			{
				while (m_offset + 1 < m_sqlSize && m_sql.at(m_offset + 1) != QLatin1Char('\n'))
					++m_offset;
				token = SqliteCompleteToken::Whitespace;
			}
			break;
		case '[':
			while (m_offset + 1 < m_sqlSize && m_sql.at(m_offset + 1) != QLatin1Char(']'))
				++m_offset;
			if (m_offset + 1 < m_sqlSize)
				++m_offset;
			else
				m_lexicallyComplete = false;
			break;
		case '`':
		case '"':
		case '\'':
		{
			const QChar quote = m_sql.at(m_offset);
			while (m_offset + 1 < m_sqlSize && m_sql.at(m_offset + 1) != quote)
				++m_offset;
			if (m_offset + 1 < m_sqlSize)
				++m_offset;
			else
				m_lexicallyComplete = false;
			break;
		}
		default:
			if (isSqliteIdentifierCodeUnit(m_sql.at(m_offset)))
			{
				const qsizetype identifierStart = m_offset;
				while (m_offset + 1 < m_sqlSize && isSqliteIdentifierCodeUnit(m_sql.at(m_offset + 1)))
					++m_offset;
				token = sqliteKeywordToken(m_sql, identifierStart, m_offset - identifierStart + 1);
			}
			break;
		}

		m_state = transitions.at(static_cast<std::size_t>(m_state)).at(static_cast<std::size_t>(token));
		if (token != SqliteCompleteToken::Whitespace && token != SqliteCompleteToken::Semicolon)
			m_statementHasToken = true;

		if (token == SqliteCompleteToken::Semicolon && m_state == State::Start)
		{
			const qsizetype statementEnd = m_offset + 1;
			m_offset                     = statementEnd;
			if (m_statementHasToken)
			{
				statement           = m_sql.sliced(m_statementStart, statementEnd - m_statementStart);
				m_statementStart    = statementEnd;
				m_statementHasToken = false;
				return true;
			}
			m_statementStart = statementEnd;
			--m_offset;
		}
	}

	m_exhausted = true;
	if (!m_statementHasToken)
		return false;

	statement           = m_sql.sliced(m_statementStart, m_sqlSize - m_statementStart);
	m_statementHasToken = false;
	return true;
}

bool SqliteStatementScanner::complete()
{
	QStringView statement;
	while (next(statement))
	{
	}
	return m_lexicallyComplete && m_state == State::Start;
}

bool sqliteStatementIsComplete(const QStringView sql)
{
	SqliteStatementScanner scanner(sql);
	return scanner.complete();
}
