/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: HyperlinkActionUtils.cpp
 * Role: Stateless helpers for decoding hyperlink action text and selecting hyperlink dispatch behavior.
 */

#include "HyperlinkActionUtils.h"

#include <QUrl>
#include <algorithm>

namespace
{
	constexpr qsizetype kPluginIdLength = 24;

	bool                isHexPluginId(const QStringView pluginId)
	{
		if (pluginId.size() != kPluginIdLength)
			return false;

		return std::ranges::all_of(pluginId,
		                           [](const QChar ch)
		                           {
			                           const QChar lower = ch.toLower();
			                           return ch.isDigit() ||
			                                  (lower >= QLatin1Char('a') && lower <= QLatin1Char('f'));
		                           });
	}

	bool isValidRoutineName(const QStringView routine)
	{
		if (routine.isEmpty())
			return false;
		return std::ranges::all_of(
		    routine, [](const QChar ch)
		    { return ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('.'); });
	}
} // namespace

QString decodeMxpActionText(QString text)
{
	text = QUrl::fromPercentEncoding(text.toUtf8());
	if (!text.contains(QLatin1Char('&')))
		return text;

	QString result;
	result.reserve(text.size());
	for (qsizetype i = 0; i < text.size(); ++i)
	{
		const QChar ch = text.at(i);
		if (ch != QLatin1Char('&'))
		{
			result.append(ch);
			continue;
		}

		const qsizetype semi = text.indexOf(QLatin1Char(';'), i + 1);
		if (semi < 0)
		{
			result.append(ch);
			continue;
		}

		const QString entity = text.mid(i + 1, semi - i - 1).trimmed();
		QString       decoded;
		if (entity.compare(QStringLiteral("quot"), Qt::CaseInsensitive) == 0)
			decoded = QStringLiteral("\"");
		else if (entity.compare(QStringLiteral("apos"), Qt::CaseInsensitive) == 0)
			decoded = QStringLiteral("'");
		else if (entity.compare(QStringLiteral("amp"), Qt::CaseInsensitive) == 0)
			decoded = QStringLiteral("&");
		else if (entity.compare(QStringLiteral("lt"), Qt::CaseInsensitive) == 0)
			decoded = QStringLiteral("<");
		else if (entity.compare(QStringLiteral("gt"), Qt::CaseInsensitive) == 0)
			decoded = QStringLiteral(">");
		else if (entity.startsWith(QLatin1Char('#')))
		{
			bool     ok   = false;
			uint32_t code = 0;
			if (entity.size() > 2 && (entity.at(1) == QLatin1Char('x') || entity.at(1) == QLatin1Char('X')))
				code = entity.mid(2).toUInt(&ok, 16);
			else
				code = entity.mid(1).toUInt(&ok, 10);
			if (ok && code <= 0x10FFFFu)
				decoded = QString(QChar::fromUcs4(code));
		}

		if (decoded.isEmpty())
		{
			result.append(ch);
			continue;
		}

		result.append(decoded);
		i = semi;
	}
	return result;
}

QString normalizeMxpActionText(const QString &text)
{
	return decodeMxpActionText(text).trimmed();
}

QString firstMxpSendAction(const QString &href)
{
	const QStringList parts = href.split(QLatin1Char('|'), Qt::KeepEmptyParts);
	for (const QString &part : parts)
	{
		const QString trimmed = part.trimmed();
		if (!trimmed.isEmpty())
			return trimmed;
	}
	return href.trimmed();
}

bool parsePluginHyperlinkCall(const QString &action, PluginHyperlinkCall &parsed)
{
	const QString payload = action.trimmed();
	if (payload.size() < kPluginIdLength + 5 || !payload.startsWith(QStringLiteral("!!")) ||
	    !payload.endsWith(QLatin1Char(')')))
		return false;

	const QStringView payloadView{payload};
	if (payloadView.at(2 + kPluginIdLength) != QLatin1Char(':'))
		return false;

	const QStringView pluginId = payloadView.mid(2, kPluginIdLength);
	if (!isHexPluginId(pluginId))
		return false;

	const QStringView callback = payloadView.mid(3 + kPluginIdLength);
	const qsizetype   bracket  = callback.indexOf(QLatin1Char('('));
	if (bracket <= 0)
		return false;

	const QStringView routine = callback.first(bracket);
	if (!isValidRoutineName(routine))
		return false;

	const QStringView argumentWithClose = callback.mid(bracket + 1);
	if (!argumentWithClose.endsWith(QLatin1Char(')')))
		return false;

	parsed.pluginId = pluginId.toString();
	parsed.routine  = routine.toString();
	parsed.argument = argumentWithClose.first(argumentWithClose.size() - 1).toString();
	return true;
}

MxpHyperlinkDispatchPolicy resolveMxpHyperlinkDispatchPolicy(const QVector<WorldRuntime::LineEntry> &lines,
                                                             const QString &normalizedHref)
{
	for (qsizetype i = lines.size(); i > 0; --i)
	{
		const WorldRuntime::LineEntry &line = lines.at(i - 1);
		for (const WorldRuntime::StyleSpan &span : line.spans)
		{
			if (decodeMxpActionText(span.action) != normalizedHref)
				continue;
			if (span.actionType == WorldRuntime::ActionPrompt)
				return MxpHyperlinkDispatchPolicy::PromptInput;
			if (span.actionType == WorldRuntime::ActionSend && (line.flags & WorldRuntime::LineNote) != 0)
				return MxpHyperlinkDispatchPolicy::CommandProcessing;
			break;
		}
	}

	return MxpHyperlinkDispatchPolicy::DirectSend;
}
