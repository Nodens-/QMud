/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: XmlEscapeUtils.h
 * Role: Shared single-pass XML attribute and element-text escaping for persistence.
 */
#ifndef QMUD_XMLESCAPEUTILS_H
#define QMUD_XMLESCAPEUTILS_H
#include <QString>
#include <QStringView>
namespace QMudXmlEscapeUtils
{
	/** @brief Selects the established XML attribute or element-text escaping rules. */
	enum class XmlEscapeContext
	{
		Attribute,
		Text
	};

	/**
	 * @brief Escapes changed XML spans in one pass, sharing inputs that need no changes.
	 * @param source Unescaped UTF-16 input.
	 * @param context Attribute or element-text escaping rules.
	 * @return Escaped XML preserving the existing whitespace and character handling.
	 */
	inline QString escapeXml(const QString &source, const XmlEscapeContext context)
	{
		QString           result;
		const QStringView view(source);
		qsizetype         runStart = 0;
		bool              changed  = false;
		for (qsizetype index = 0; index < view.size(); ++index)
		{
			const ushort      code = view.at(index).unicode();
			QLatin1StringView replacement;
			switch (code)
			{
			case '<':
				replacement = QLatin1StringView("&lt;");
				break;
			case '>':
				replacement = QLatin1StringView("&gt;");
				break;
			case '&':
				replacement = QLatin1StringView("&amp;");
				break;
			case '"':
				if (context != XmlEscapeContext::Attribute)
					continue;
				replacement = QLatin1StringView("&quot;");
				break;
			case '\t':
				if (context != XmlEscapeContext::Text)
					continue;
				replacement = QLatin1StringView("&#9;");
				break;
			default:
				if (code >= 0x20 || code == '\n' || code == '\r')
					continue;
				break;
			}
			if (!changed)
			{
				result.reserve(source.size());
				changed = true;
			}
			result += view.sliced(runStart, index - runStart);
			result += replacement;
			runStart = index + 1;
		}
		if (!changed)
			return source;
		result += view.sliced(runStart);
		return result;
	}

} // namespace QMudXmlEscapeUtils
#endif
