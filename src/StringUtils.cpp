/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: StringUtils.cpp
 * Role: String and command-name utility implementations used by Lua bindings, command dispatch, and text
 * normalization helpers.
 */

#include "StringUtils.h"
#include "CommandMappingTypes.h"

#include <QByteArray>
#include <QHash>
#include <QVector>

namespace
{
	int hexDigitValue(const char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	}

	struct CommandLookupTables
	{
			QHash<int, QString> idToName;
			QHash<QString, int> nameToId;
	};

	const CommandLookupTables &commandLookupTables()
	{
		static const CommandLookupTables tables = []
		{
			CommandLookupTables lookup;
			for (int i = 0; kCommandIdMappingTable[i].commandId; ++i)
			{
				const int     id   = kCommandIdMappingTable[i].commandId;
				const QString name = QString::fromLatin1(kCommandIdMappingTable[i].commandName);
				if (!lookup.idToName.contains(id))
					lookup.idToName.insert(id, name);
				const QString lowerName = name.toLower();
				if (!lookup.nameToId.contains(lowerName))
					lookup.nameToId.insert(lowerName, id);
			}
			return lookup;
		}();
		return tables;
	}
} // namespace

QString qmudCommandIdToString(const int id)
{
	if (!id)
		return {};
	const auto &lookup = commandLookupTables();
	const auto  it     = lookup.idToName.constFind(id);
	return it != lookup.idToName.cend() ? *it : QString();
}

int qmudStringToCommandId(const QString &command)
{
	if (command.isEmpty())
		return 0;

	const auto &lookup = commandLookupTables();
	const auto  it     = lookup.nameToId.constFind(command.toLower());
	return it != lookup.nameToId.cend() ? *it : 0;
}

QString qmudFixupEscapeSequences(const QString &source)
{
	const QByteArray bytes = source.toLocal8Bit();
	QByteArray       out;
	out.reserve(bytes.size());
	const char *p = bytes.constData();

	for (; *p; ++p)
	{
		char c = *p;
		if (c == '\\')
		{
			c = *(++p);
			switch (c)
			{
			case 'a':
				c = '\a';
				break;
			case 'b':
				c = '\b';
				break;
			case 'f':
				c = '\f';
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case 'v':
				c = '\v';
				break;
			case '\'':
				c = '\'';
				break;
			case '\"':
				c = '\"';
				break;
			case '\\':
				c = '\\';
				break;
			case '?':
				c = '\?';
				break;
			case 'x':
				c = 0;
				++p;
				for (int i = 0; *p && i < 2; ++p, ++i)
				{
					const int digit = hexDigitValue(*p);
					if (digit < 0)
						break;
					c = static_cast<char>((c << 4) + digit);
				}
				--p;
				break;
			default:
				break;
			}
		}
		out.append(c);
	}

	return QString::fromLocal8Bit(out);
}

QString qmudReplaceString(const QString &source, const QString &target, const QString &replacement,
                          const bool replaceAll)
{
	if (target.isEmpty())
		return source;

	QString result = source;
	if (replaceAll)
	{
		result.replace(target, replacement);
		return result;
	}

	const qsizetype index = result.indexOf(target);
	if (index >= 0)
		result.replace(index, target.size(), replacement);
	return result;
}

int qmudEditDistance(const QStringView source, const QStringView target)
{
	constexpr qsizetype kMaxLength = 20;
	const qsizetype     n          = qMin(source.size(), kMaxLength);
	const qsizetype     m          = qMin(target.size(), kMaxLength);

	if (n == 0)
		return static_cast<int>(m);
	if (m == 0)
		return static_cast<int>(n);

	QVector<int> previous(m + 1);
	QVector<int> current(m + 1);

	for (qsizetype j = 0; j <= m; ++j)
		previous[j] = static_cast<int>(j);

	for (qsizetype i = 0; i < n; ++i)
	{
		current[0] = static_cast<int>(i + 1);
		for (qsizetype j = 0; j < m; ++j)
		{
			const int cost = (source.at(i) == target.at(j)) ? 0 : 1;
			current[j + 1] = qMin(qMin(current[j] + 1, previous[j + 1] + 1), previous[j] + cost);
		}
		previous.swap(current);
	}

	return previous[m];
}

QString qmudFixUpGerman(const QString &message)
{
	QString result = message;
	result.replace(QStringLiteral("ü"), QStringLiteral("ue"));
	result.replace(QStringLiteral("Ü"), QStringLiteral("Ue"));
	result.replace(QStringLiteral("ä"), QStringLiteral("ae"));
	result.replace(QStringLiteral("Ä"), QStringLiteral("Ae"));
	result.replace(QStringLiteral("ö"), QStringLiteral("oe"));
	result.replace(QStringLiteral("Ö"), QStringLiteral("Oe"));
	result.replace(QStringLiteral("ß"), QStringLiteral("ss"));
	return result;
}

bool qmudIsEnabledFlag(const QString &value)
{
	return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
	       value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0 ||
	       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

QString qmudBoolToYn(const bool value)
{
	return value ? QStringLiteral("y") : QStringLiteral("n");
}
