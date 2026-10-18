/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MspProtocol.cpp
 * Role: Parsing and validation for MUD Sound Protocol commands.
 */

#include "MspProtocol.h"
#include "helpers/PluginPathUtils.h"

#include <QRegularExpression>
#include <algorithm>
#include <utility>

namespace
{
	using QMudMspProtocol::Command;
	using QMudMspProtocol::MediaType;

	/**
	 * @brief Parses an integer and enforces an inclusive range.
	 * @param text Text to parse.
	 * @param minimum Smallest accepted value.
	 * @param maximum Largest accepted value.
	 * @param value Destination receiving the parsed value.
	 * @return `true` when parsing and range validation succeed.
	 */
	bool parseBoundedInteger(const QString &text, const int minimum, const int maximum, int &value)
	{
		bool      ok     = false;
		const int parsed = text.trimmed().toInt(&ok);
		if (!ok || parsed < minimum || parsed > maximum)
			return false;
		value = parsed;
		return true;
	}

	/**
	 * @brief Converts a protocol media name through the shared sound-path compatibility boundary.
	 * @param fileName Media name supplied by the server.
	 * @return Portable sounds-tree-relative name, or an empty string when invalid.
	 */
	QString normalizedMediaName(const QString &fileName)
	{
		const QString trimmed = fileName.trimmed();
		if (trimmed.startsWith(QLatin1Char('/')) ||
		    trimmed.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive))
		{
			return {};
		}
		const QString relative = QMudPluginPathUtils::legacyPathRelativeToQmudHome(fileName);
		if (relative.isEmpty() || relative.contains(QLatin1Char(':')))
			return {};
		const QStringList parts = relative.split(QLatin1Char('/'), Qt::KeepEmptyParts);
		if (parts.isEmpty() || std::ranges::any_of(parts, [](const QString &part) { return part.isEmpty(); }))
		{
			return {};
		}
		for (qsizetype index = 0; index + 1 < parts.size(); ++index)
		{
			if (parts.at(index).contains(QLatin1Char('*')) || parts.at(index).contains(QLatin1Char('?')))
				return {};
		}
		const QString  &filePart = parts.constLast();
		const qsizetype star     = filePart.indexOf(QLatin1Char('*'));
		if (star >= 0 && (filePart.indexOf(QLatin1Char('*'), star + 1) >= 0 ||
		                  filePart.indexOf(QLatin1Char('?'), star + 1) >= 0))
		{
			return {};
		}
		return relative;
	}

	/**
	 * @brief Trims a protocol token and removes one matching pair of double quotes.
	 * @param value Token to normalize.
	 * @return Normalized token value.
	 */
	QString unquote(QString value)
	{
		value = value.trimmed();
		if (value.size() >= 2 && value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
			return value.mid(1, value.size() - 2);
		return value;
	}

	/**
	 * @brief Applies one validated MSP parameter to a command.
	 * @param command Command receiving the parameter.
	 * @param rawName Parameter name.
	 * @param rawValue Parameter value.
	 * @return `true` when parsing may continue; invalid standard parameters return `false`.
	 */
	bool applyParameter(Command &command, const QString &rawName, const QString &rawValue)
	{
		const QString name  = rawName.trimmed().toUpper();
		const QString value = unquote(rawValue);
		if (name == QLatin1String("V"))
			return parseBoundedInteger(value, 0, 100, command.volume);
		if (name == QLatin1String("L"))
		{
			int repeatCount = 0;
			if (!parseBoundedInteger(value, -1, 1000000, repeatCount) || repeatCount == 0)
				return false;
			command.repeatCount = repeatCount;
			return true;
		}
		if (name == QLatin1String("P"))
			return command.type == MediaType::Sound && parseBoundedInteger(value, 0, 100, command.priority);
		if (name == QLatin1String("C"))
		{
			int continueValue = 0;
			if (command.type != MediaType::Music || !parseBoundedInteger(value, 0, 1, continueValue))
				return false;
			command.continueMusic = continueValue != 0;
			return true;
		}
		if (name == QLatin1String("T"))
		{
			command.tag = value;
			return true;
		}
		if (name == QLatin1String("U"))
		{
			command.url         = value;
			command.urlProvided = true;
			return true;
		}
		return true;
	}

	/**
	 * @brief Validates the command media name and handles the protocol stop token.
	 * @param command Parsed command parameters.
	 * @param fileName Raw media-name token.
	 * @return Completed command, or no value when the media name is invalid.
	 */
	std::optional<Command> finalizeCommand(Command command, QString fileName)
	{
		fileName = unquote(std::move(fileName));
		if (fileName.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0)
		{
			command.setDefaultUrl = command.urlProvided;
			command.stop          = !command.setDefaultUrl;
			return command;
		}
		fileName = normalizedMediaName(fileName);
		if (fileName.isEmpty())
			return std::nullopt;
		command.fileName = fileName;
		return command;
	}
} // namespace

std::optional<QMudMspProtocol::Command> QMudMspProtocol::parseLine(const QString &text)
{
	static const QRegularExpression kEnvelope(QStringLiteral(R"(\A!!(SOUND|MUSIC)\(([^\r\n]*)\)\z)"),
	                                          QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression kFirstToken(QStringLiteral(R"(^\s*("[^"]*"|[^\s]+)(.*)$)"),
	                                            QRegularExpression::DotMatchesEverythingOption);
	static const QRegularExpression kParameter(
	    QStringLiteral(R"(\G\s+([A-Za-z][A-Za-z0-9_]*)\s*=\s*("[^"]*"|[^\s]+))"),
	    QRegularExpression::CaseInsensitiveOption);

	const QRegularExpressionMatch envelope = kEnvelope.match(text);
	if (!envelope.hasMatch())
		return std::nullopt;
	Command command;
	command.type = envelope.captured(1).compare(QStringLiteral("music"), Qt::CaseInsensitive) == 0
	                   ? MediaType::Music
	                   : MediaType::Sound;
	const QRegularExpressionMatch first = kFirstToken.match(envelope.captured(2));
	if (!first.hasMatch())
		return std::nullopt;
	const QString remainder = first.captured(2);
	qsizetype     offset    = 0;
	while (offset < remainder.size())
	{
		if (remainder.mid(offset).trimmed().isEmpty())
			break;
		const QRegularExpressionMatch parameter = kParameter.match(remainder, offset);
		if (!parameter.hasMatch() || parameter.capturedStart() != offset ||
		    !applyParameter(command, parameter.captured(1), parameter.captured(2)))
		{
			return std::nullopt;
		}
		offset = parameter.capturedEnd();
	}
	return finalizeCommand(std::move(command), first.captured(1));
}
