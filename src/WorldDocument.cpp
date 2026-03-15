/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldDocument.cpp
 * Role: World document serialization/model implementation that loads, stores, and mutates world configuration state.
 */

#include "WorldDocument.h"

#include "Version.h"

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>

static QString escapeInvalidAttributeChars(const QString &input)
{
	enum class State
	{
		Normal,
		Tag,
		AttrSingle,
		AttrDouble,
		Comment,
		CData,
		ProcessingInstruction,
		Doctype
	};

	auto    state = State::Normal;
	QString output;
	output.reserve(input.size());

	auto matchesInsensitive = [&input](int pos, const char *literal) -> bool
	{
		const QLatin1StringView token(literal);
		return input.mid(pos, token.size()).compare(token, Qt::CaseInsensitive) == 0;
	};

	for (int i = 0; i < input.size();)
	{
		const QChar ch = input.at(i);
		switch (state)
		{
		case State::Normal:
			if (ch == QLatin1Char('<'))
			{
				if (matchesInsensitive(i, "<![CDATA["))
					state = State::CData;
				else if (matchesInsensitive(i, "<!--"))
					state = State::Comment;
				else if (matchesInsensitive(i, "<?"))
					state = State::ProcessingInstruction;
				else if (matchesInsensitive(i, "<!DOCTYPE"))
					state = State::Doctype;
				else
					state = State::Tag;
			}
			output += ch;
			++i;
			break;
		case State::Tag:
			if (ch == QLatin1Char('"'))
				state = State::AttrDouble;
			else if (ch == QLatin1Char('\''))
				state = State::AttrSingle;
			else if (ch == QLatin1Char('>'))
				state = State::Normal;
			output += ch;
			++i;
			break;
		case State::AttrDouble:
			if (ch == QLatin1Char('"'))
			{
				state = State::Tag;
				output += ch;
				++i;
				break;
			}
			if (ch == QLatin1Char('<'))
			{
				output += QStringLiteral("&lt;");
				++i;
				break;
			}
			output += ch;
			++i;
			break;
		case State::AttrSingle:
			if (ch == QLatin1Char('\''))
			{
				state = State::Tag;
				output += ch;
				++i;
				break;
			}
			if (ch == QLatin1Char('<'))
			{
				output += QStringLiteral("&lt;");
				++i;
				break;
			}
			output += ch;
			++i;
			break;
		case State::Comment:
			if (i + 2 < input.size() && input.mid(i, 3) == QLatin1String("-->"))
			{
				output += QStringLiteral("-->");
				i += 3;
				state = State::Normal;
				break;
			}
			output += ch;
			++i;
			break;
		case State::CData:
			if (i + 2 < input.size() && input.mid(i, 3) == QLatin1String("]]>"))
			{
				output += QStringLiteral("]]>");
				i += 3;
				state = State::Normal;
				break;
			}
			output += ch;
			++i;
			break;
		case State::ProcessingInstruction:
			if (i + 1 < input.size() && input.mid(i, 2) == QLatin1String("?>"))
			{
				output += QStringLiteral("?>");
				i += 2;
				state = State::Normal;
				break;
			}
			output += ch;
			++i;
			break;
		case State::Doctype:
			if (ch == QLatin1Char('>'))
				state = State::Normal;
			output += ch;
			++i;
			break;
		}
	}

	return output;
}

static QString wrapScriptBlocksInCData(const QString &input)
{
	qsizetype pos = 0;
	QString output;
	output.reserve(input.size());
	const auto startTag = QStringLiteral("<script");
	const auto endTag   = QStringLiteral("</script>");
	while (true)
	{
		const qsizetype start = input.indexOf(startTag, pos, Qt::CaseInsensitive);
		if (start < 0)
		{
			output += input.mid(pos);
			break;
		}
		const qsizetype startTagEnd = input.indexOf(QLatin1Char('>'), start);
		if (startTagEnd < 0)
		{
			output += input.mid(pos);
			break;
		}
		output += input.mid(pos, startTagEnd - pos + 1);
		const qsizetype end = input.indexOf(endTag, startTagEnd + 1, Qt::CaseInsensitive);
		if (end < 0)
		{
			output += input.mid(startTagEnd + 1);
			break;
		}
		QString body = input.mid(startTagEnd + 1, end - (startTagEnd + 1));
		if (body.contains(QStringLiteral("<![CDATA[")))
		{
			output += body;
		}
		else
		{
			body.replace(QStringLiteral("]]>"), QStringLiteral("]]]]><![CDATA[>"));
			output += QStringLiteral("<![CDATA[");
			output += body;
			output += QStringLiteral("]]>");
		}
		output += input.mid(end, endTag.size());
		pos = end + endTag.size();
	}
	return output;
}

static QString sanitizeXmlForQt(const QString &input)
{
	return wrapScriptBlocksInCData(escapeInvalidAttributeChars(input));
}

static QStringList macroDescriptionList()
{
	return {QStringLiteral("up"),        QStringLiteral("down"),      QStringLiteral("north"),
	        QStringLiteral("south"),     QStringLiteral("east"),      QStringLiteral("west"),
	        QStringLiteral("examine"),   QStringLiteral("look"),      QStringLiteral("page"),
	        QStringLiteral("say"),       QStringLiteral("whisper"),   QStringLiteral("doing"),
	        QStringLiteral("who"),       QStringLiteral("drop"),      QStringLiteral("take"),
	        QStringLiteral("F2"),        QStringLiteral("F3"),        QStringLiteral("F4"),
	        QStringLiteral("F5"),        QStringLiteral("F7"),        QStringLiteral("F8"),
	        QStringLiteral("F9"),        QStringLiteral("F10"),       QStringLiteral("F11"),
	        QStringLiteral("F12"),       QStringLiteral("F2+Shift"),  QStringLiteral("F3+Shift"),
	        QStringLiteral("F4+Shift"),  QStringLiteral("F5+Shift"),  QStringLiteral("F6+Shift"),
	        QStringLiteral("F7+Shift"),  QStringLiteral("F8+Shift"),  QStringLiteral("F9+Shift"),
	        QStringLiteral("F10+Shift"), QStringLiteral("F11+Shift"), QStringLiteral("F12+Shift"),
	        QStringLiteral("F2+Ctrl"),   QStringLiteral("F3+Ctrl"),   QStringLiteral("F5+Ctrl"),
	        QStringLiteral("F7+Ctrl"),   QStringLiteral("F8+Ctrl"),   QStringLiteral("F9+Ctrl"),
	        QStringLiteral("F10+Ctrl"),  QStringLiteral("F11+Ctrl"),  QStringLiteral("F12+Ctrl"),
	        QStringLiteral("logout"),    QStringLiteral("quit"),      QStringLiteral("Alt+A"),
	        QStringLiteral("Alt+B"),     QStringLiteral("Alt+J"),     QStringLiteral("Alt+K"),
	        QStringLiteral("Alt+L"),     QStringLiteral("Alt+M"),     QStringLiteral("Alt+N"),
	        QStringLiteral("Alt+O"),     QStringLiteral("Alt+P"),     QStringLiteral("Alt+Q"),
	        QStringLiteral("Alt+R"),     QStringLiteral("Alt+S"),     QStringLiteral("Alt+T"),
	        QStringLiteral("Alt+U"),     QStringLiteral("Alt+X"),     QStringLiteral("Alt+Y"),
	        QStringLiteral("Alt+Z"),     QStringLiteral("F1"),        QStringLiteral("F1+Ctrl"),
	        QStringLiteral("F1+Shift"),  QStringLiteral("F6"),        QStringLiteral("F6+Ctrl")};
}

static QStringList keypadNameList()
{
	return {QStringLiteral("0"),      QStringLiteral("1"),      QStringLiteral("2"),
	        QStringLiteral("3"),      QStringLiteral("4"),      QStringLiteral("5"),
	        QStringLiteral("6"),      QStringLiteral("7"),      QStringLiteral("8"),
	        QStringLiteral("9"),      QStringLiteral("."),      QStringLiteral("/"),
	        QStringLiteral("*"),      QStringLiteral("-"),      QStringLiteral("+"),
	        QStringLiteral("Ctrl+0"), QStringLiteral("Ctrl+1"), QStringLiteral("Ctrl+2"),
	        QStringLiteral("Ctrl+3"), QStringLiteral("Ctrl+4"), QStringLiteral("Ctrl+5"),
	        QStringLiteral("Ctrl+6"), QStringLiteral("Ctrl+7"), QStringLiteral("Ctrl+8"),
	        QStringLiteral("Ctrl+9"), QStringLiteral("Ctrl+."), QStringLiteral("Ctrl+/"),
	        QStringLiteral("Ctrl+*"), QStringLiteral("Ctrl+-"), QStringLiteral("Ctrl++")};
}

static QString withLine(const QXmlStreamReader &reader, const QString &message)
{
	const qint64 line = reader.lineNumber();
	if (line > 0)
		return QStringLiteral("Line %1: %2").arg(line).arg(message);
	return message;
}

WorldDocument::WorldDocument(QObject *parent) : QObject(parent)
{
}

void WorldDocument::clearState()
{
	m_errorString.clear();
	m_worldFileVersion = 0;
	m_qmudVersion.clear();
	m_dateSaved = QDateTime();
	m_worldAttributes.clear();
	m_worldMultilineAttributes.clear();
	m_triggers.clear();
	m_aliases.clear();
	m_timers.clear();
	m_macros.clear();
	m_variables.clear();
	m_colours.clear();
	m_keypadEntries.clear();
	m_printingStyles.clear();
	m_comments.clear();
	m_plugins.clear();
	m_loadedPluginIds.clear();
	m_includes.clear();
	m_scripts.clear();
	m_currentIncludeStack.clear();
	m_includeFileList.clear();
	m_warnings.clear();
	m_pluginFile             = false;
	m_pluginContentFinalized = false;
}

bool WorldDocument::loadFromFile(const QString &fileName)
{
	return loadFromFileWithPolicy(fileName, PluginPolicy::AllowPlugins, false);
}

bool WorldDocument::loadFromPluginFile(const QString &fileName)
{
	return loadFromFileWithPolicy(fileName, PluginPolicy::RequirePlugin, false);
}

bool WorldDocument::loadFromFileWithPolicy(const QString &fileName, PluginPolicy policy, bool includeContext)
{
	clearState();
	m_loadedFromInclude      = includeContext;
	m_pluginFile             = (policy == PluginPolicy::RequirePlugin);
	m_pluginContentFinalized = false;

	QFile file(fileName);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		m_errorString = QStringLiteral("Unable to open world file: %1").arg(fileName);
		return false;
	}

	if (!isArchiveXMLFile(fileName))
	{
		m_errorString = QStringLiteral("File \"%1\" is not an XML file").arg(fileName);
		return false;
	}

	// XML load path using QXmlStreamReader over sanitized input text.
	const QByteArray rawBytes  = file.readAll();
	const QString    xmlText   = QString::fromUtf8(rawBytes);
	const QString    sanitized = sanitizeXmlForQt(xmlText);
	QXmlStreamReader reader(sanitized);

	bool             sawContent = false;

	auto             parseChildren = [&reader]() -> QMap<QString, QString>
	{
		QMap<QString, QString> children;
		while (!reader.atEnd())
		{
			reader.readNext();
			if (reader.isStartElement())
			{
				const QString childName = reader.name().toString();
				const QString childText = reader.readElementText(QXmlStreamReader::IncludeChildElements);
				children.insert(childName, childText);
			}
			else if (reader.isEndElement())
			{
				break;
			}
		}
		return children;
	};

	while (!reader.atEnd())
	{
		reader.readNext();

		if (reader.isStartElement())
		{
			const QString name = reader.name().toString();

			if (name == QLatin1String("muclient") || name == QLatin1String("qmud"))
			{
				sawContent = true;
			}
			else if (name == QLatin1String("world"))
			{
				sawContent = true;

					const QXmlStreamAttributes attrs = reader.attributes();
					for (const QXmlStreamAttribute &attr : attrs)
					{
						const QString attrName  = attr.name().toString();
						const QString attrValue = attr.value().toString();
						if (attrName == QLatin1String("muclient_version"))
						{
							if (!attrs.hasAttribute(QLatin1String("qmud_version")))
								m_worldAttributes.insert(QStringLiteral("qmud_version"), attrValue);
							continue;
						}
						m_worldAttributes.insert(attrName, attrValue);
					}

					if (attrs.hasAttribute(QLatin1String("world_file_version")))
						m_worldFileVersion = attrs.value(QLatin1String("world_file_version")).toInt();
					if (attrs.hasAttribute(QLatin1String("qmud_version")))
						m_qmudVersion = attrs.value(QLatin1String("qmud_version")).toString();
					else if (attrs.hasAttribute(QLatin1String("muclient_version")))
						m_qmudVersion = attrs.value(QLatin1String("muclient_version")).toString();
				if (attrs.hasAttribute(QLatin1String("date_saved")))
				{
					const QString dateText = attrs.value(QLatin1String("date_saved")).toString();
					m_dateSaved = QDateTime::fromString(dateText, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
				}

				// General attributes are on the <world> element.
				// Multi-line alpha options are child elements (eg. <notes>...</notes>).
				while (!reader.atEnd())
				{
					reader.readNext();
					if (reader.isStartElement())
					{
						const QString childName = reader.name().toString();
						const QString childText =
						    reader.readElementText(QXmlStreamReader::IncludeChildElements);
						m_worldMultilineAttributes.insert(childName, childText);
					}
					else if (reader.isEndElement() && reader.name() == QLatin1String("world"))
						break;
				}
			}
			else if (name == QLatin1String("triggers"))
			{
				sawContent = true;
				// Collect trigger elements and their contents.
				while (!reader.atEnd())
				{
					reader.readNext();
					if (reader.isStartElement() && reader.name() == QLatin1String("trigger"))
					{
						Trigger                    trigger;
						const QXmlStreamAttributes attrs = reader.attributes();
						for (const QXmlStreamAttribute &attr : attrs)
						{
							trigger.attributes.insert(attr.name().toString(), attr.value().toString());
						}
						// child elements (eg. <send>...</send>)
						trigger.children = parseChildren();
						trigger.included = m_loadedFromInclude;
						m_triggers.push_back(trigger);
					}
					else if (reader.isEndElement() && reader.name() == QLatin1String("triggers"))
						break;
				}
			}
			else if (name == QLatin1String("aliases"))
			{
				sawContent = true;
				// Collect alias elements and their contents.
				while (!reader.atEnd())
				{
					reader.readNext();
					if (reader.isStartElement() && reader.name() == QLatin1String("alias"))
					{
						Alias                      alias;
						const QXmlStreamAttributes attrs = reader.attributes();
						for (const QXmlStreamAttribute &attr : attrs)
						{
							alias.attributes.insert(attr.name().toString(), attr.value().toString());
						}
						// child elements (eg. <send>...</send>)
						alias.children = parseChildren();
						alias.included = m_loadedFromInclude;
						m_aliases.push_back(alias);
					}
					else if (reader.isEndElement() && reader.name() == QLatin1String("aliases"))
						break;
				}
			}
			else if (name == QLatin1String("timers"))
			{
				sawContent = true;
				// Collect timer elements and their contents.
				while (!reader.atEnd())
				{
					reader.readNext();
					if (reader.isStartElement() && reader.name() == QLatin1String("timer"))
					{
						Timer                      timer;
						const QXmlStreamAttributes attrs = reader.attributes();
						for (const QXmlStreamAttribute &attr : attrs)
						{
							timer.attributes.insert(attr.name().toString(), attr.value().toString());
						}
						// child elements (eg. <send>...</send>)
						timer.children = parseChildren();
						timer.included = m_loadedFromInclude;
						m_timers.push_back(timer);
					}
					else if (reader.isEndElement() && reader.name() == QLatin1String("timers"))
						break;
				}
			}
			else if (name == QLatin1String("macros"))
			{
				sawContent = true;
				// Collect macro elements and their contents.
				const QStringList macroNames = macroDescriptionList();
				while (!reader.atEnd())
				{
					reader.readNext();
					if (reader.isStartElement() && reader.name() == QLatin1String("macro"))
					{
						Macro                      macro;
						const QXmlStreamAttributes attrs = reader.attributes();
						for (const QXmlStreamAttribute &attr : attrs)
						{
							macro.attributes.insert(attr.name().toString(), attr.value().toString());
						}
						// child elements (eg. <send>...</send>)
						macro.children          = parseChildren();
						const QString macroName = macro.attributes.value(QStringLiteral("name")).trimmed();
						if (!macroNames.contains(macroName))
						{
							m_warnings.push_back(withLine(
							    reader, QStringLiteral("Macro named \"%1\" not recognised").arg(macroName)));
							continue;
						}
						const QString macroType = macro.attributes.value(QStringLiteral("type")).trimmed();
						if (macroType.isEmpty())
						{
							m_warnings.push_back(
							    withLine(reader, QStringLiteral("Macro type must be specified")));
							continue;
						}
						if (macroType != QStringLiteral("replace") &&
						    macroType != QStringLiteral("send_now") && macroType != QStringLiteral("insert"))
						{
							m_warnings.push_back(withLine(
							    reader, QStringLiteral("Macro type \"%1\" not recognised").arg(macroType)));
							continue;
						}
						m_macros.push_back(macro);
					}
					else if (reader.isEndElement() && reader.name() == QLatin1String("macros"))
						break;
				}
			}
			else if (name == QLatin1String("variables"))
			{
				sawContent = true;
				// Collect variable elements and their contents.
				while (!reader.atEnd())
				{
					reader.readNext();
					if (reader.isStartElement() && reader.name() == QLatin1String("variable"))
					{
						Variable                   variable;
						const QXmlStreamAttributes attrs = reader.attributes();
						for (const QXmlStreamAttribute &attr : attrs)
						{
							variable.attributes.insert(attr.name().toString(), attr.value().toString());
						}
						variable.content = reader.readElementText(QXmlStreamReader::IncludeChildElements);
						m_variables.push_back(variable);
					}
					else if (reader.isEndElement() && reader.name() == QLatin1String("variables"))
						break;
				}
			}
			else if (name == QLatin1String("colours"))
			{
				sawContent = true;
				// Collect colour elements by group.
				while (!reader.atEnd())
				{
					reader.readNext();
					if (reader.isStartElement())
					{
						const QString colourGroup = reader.name().toString();
						if (colourGroup == QLatin1String("ansi") || colourGroup == QLatin1String("custom"))
						{
							while (!reader.atEnd())
							{
								reader.readNext();
								if (reader.isStartElement())
								{
									const QString subGroup = reader.name().toString();
									if (subGroup == QLatin1String("normal") ||
									    subGroup == QLatin1String("bold") ||
									    subGroup == QLatin1String("custom"))
									{
										while (!reader.atEnd())
										{
											reader.readNext();
											if (reader.isStartElement() &&
											    reader.name() == QLatin1String("colour"))
											{
												Colour colour;
												colour.group = colourGroup + QStringLiteral("/") + subGroup;
												const QXmlStreamAttributes attrs = reader.attributes();
												for (const QXmlStreamAttribute &attr : attrs)
												{
													colour.attributes.insert(attr.name().toString(),
													                         attr.value().toString());
												}
												const QString seqText =
												    colour.attributes.value(QStringLiteral("seq")).trimmed();
												bool      ok  = false;
												const int seq = seqText.toInt(&ok);
												const int maxSeq =
												    colourGroup == QLatin1String("custom") ? 16 : 8;
												if (!ok || seq < 1 || seq > maxSeq)
												{
													m_warnings.push_back(withLine(
													    reader,
													    QStringLiteral(
													        "Colour sequence (\"seq\") must be specified")));
													continue;
												}
												m_colours.push_back(colour);
											}
											else if (reader.isEndElement() && reader.name() == subGroup)
												break;
										}
									}
									else if (subGroup == QLatin1String("colour"))
									{
										Colour colour;
										colour.group                     = colourGroup;
										const QXmlStreamAttributes attrs = reader.attributes();
										for (const QXmlStreamAttribute &attr : attrs)
										{
											colour.attributes.insert(attr.name().toString(),
											                         attr.value().toString());
										}
										const QString seqText =
										    colour.attributes.value(QStringLiteral("seq")).trimmed();
										bool      ok     = false;
										const int seq    = seqText.toInt(&ok);
										const int maxSeq = colourGroup == QLatin1String("custom") ? 16 : 8;
										if (!ok || seq < 1 || seq > maxSeq)
										{
											m_warnings.push_back(withLine(
											    reader, QStringLiteral(
											                "Colour sequence (\"seq\") must be specified")));
											continue;
										}
										m_colours.push_back(colour);
									}
								}
								else if (reader.isEndElement() && reader.name() == colourGroup)
									break;
							}
						}
					}
					else if (reader.isEndElement() && reader.name() == QLatin1String("colours"))
						break;
				}
			}
			else if (name == QLatin1String("keypad"))
			{
				sawContent = true;
				// Collect key elements and their contents.
				const QStringList keypadNames = keypadNameList();
				while (!reader.atEnd())
				{
					reader.readNext();
					if (reader.isStartElement() && reader.name() == QLatin1String("key"))
					{
						Keypad                     key;
						const QXmlStreamAttributes attrs = reader.attributes();
						for (const QXmlStreamAttribute &attr : attrs)
						{
							key.attributes.insert(attr.name().toString(), attr.value().toString());
						}
						const QMap<QString, QString> children = parseChildren();
						key.content                           = children.value(QStringLiteral("send"));
						if (key.content.isEmpty())
							key.content = children.value(QStringLiteral("text"));
						const QString keyName = key.attributes.value(QStringLiteral("name")).trimmed();
						if (!keypadNames.contains(keyName))
						{
							m_warnings.push_back(withLine(
							    reader, QStringLiteral("Key named \"%1\" not recognised").arg(keyName)));
							continue;
						}
						m_keypadEntries.push_back(key);
					}
					else if (reader.isEndElement() && reader.name() == QLatin1String("keypad"))
						break;
				}
			}
			else if (name == QLatin1String("printing"))
			{
				sawContent = true;
				// Collect style elements by group.
				constexpr int kPrintStyleCount = 8;
				while (!reader.atEnd())
				{
					reader.readNext();
					if (reader.isStartElement())
					{
						const QString printingGroup = reader.name().toString();
						if (printingGroup == QLatin1String("ansi"))
						{
							while (!reader.atEnd())
							{
								reader.readNext();
								if (reader.isStartElement())
								{
									const QString subGroup = reader.name().toString();
									if (subGroup == QLatin1String("normal") ||
									    subGroup == QLatin1String("bold"))
									{
										while (!reader.atEnd())
										{
											reader.readNext();
											if (reader.isStartElement() &&
											    reader.name() == QLatin1String("style"))
											{
												PrintingStyle style;
												style.group = printingGroup + QStringLiteral("/") + subGroup;
												const QXmlStreamAttributes attrs = reader.attributes();
												for (const QXmlStreamAttribute &attr : attrs)
												{
													style.attributes.insert(attr.name().toString(),
													                        attr.value().toString());
												}
												const QString seqText =
												    style.attributes.value(QStringLiteral("seq")).trimmed();
												bool      ok  = false;
												const int seq = seqText.toInt(&ok);
												if (!ok || seq < 1 || seq > kPrintStyleCount)
												{
													m_warnings.push_back(withLine(
													    reader,
													    QStringLiteral(
													        "Style sequence (\"seq\") must be specified")));
													continue;
												}
												m_printingStyles.push_back(style);
											}
											else if (reader.isEndElement() && reader.name() == subGroup)
												break;
										}
									}
								}
								else if (reader.isEndElement() && reader.name() == printingGroup)
									break;
							}
						}
					}
					else if (reader.isEndElement() && reader.name() == QLatin1String("printing"))
						break;
				}
			}
			else if (name == QLatin1String("comment"))
			{
				sawContent = true;
				// Collect comment content.
				const QString commentText = reader.readElementText(QXmlStreamReader::IncludeChildElements);
				if (!commentText.isEmpty())
				{
					if (!m_comments.isEmpty())
						m_comments += "\n";
					m_comments += commentText;
				}
			}
			else if (name == QLatin1String("plugin"))
			{
				sawContent = true;
				// Collect plugin attributes, description, and script content.
				Plugin                     plugin;
				const QXmlStreamAttributes attrs = reader.attributes();
				for (const QXmlStreamAttribute &attr : attrs)
				{
					plugin.attributes.insert(attr.name().toString(), attr.value().toString());
				}

				while (!reader.atEnd())
				{
					reader.readNext();
					if (reader.isStartElement())
					{
						const QString childName = reader.name().toString();
						if (childName == QLatin1String("description"))
							plugin.description =
							    reader.readElementText(QXmlStreamReader::IncludeChildElements);
						else if (childName == QLatin1String("script"))
						{
							if (!plugin.script.isEmpty())
								plugin.script += "\n";
							plugin.script += reader.readElementText(QXmlStreamReader::IncludeChildElements);
						}
						else
						{
							static const QHash<QString, QString> kForbiddenPluginChildTagWarnings = {
							    {QStringLiteral("macros"),
							     QStringLiteral("<macros> tag cannot be used inside a plugin")  },
							    {QStringLiteral("colours"),
							     QStringLiteral("<colours> tag cannot be used inside a plugin") },
							    {QStringLiteral("keypad"),
							     QStringLiteral("<keypad> tag cannot be used inside a plugin")  },
							    {QStringLiteral("printing"),
							     QStringLiteral("<printing> tag cannot be used inside a plugin")}
                            };
							const auto forbiddenTagWarning =
							    kForbiddenPluginChildTagWarnings.constFind(childName);
							if (forbiddenTagWarning != kForbiddenPluginChildTagWarnings.constEnd())
								m_warnings.push_back(withLine(reader, *forbiddenTagWarning));
							reader.readElementText(QXmlStreamReader::IncludeChildElements);
						}
					}
					else if (reader.isEndElement() && reader.name() == QLatin1String("plugin"))
						break;
				}

				m_plugins.push_back(plugin);
			}
			else if (name == QLatin1String("include"))
			{
				sawContent = true;
				// Collect include attributes.
				Include                    include;
				const QXmlStreamAttributes attrs = reader.attributes();
				for (const QXmlStreamAttribute &attr : attrs)
				{
					include.attributes.insert(attr.name().toString(), attr.value().toString());
				}
				// consume any content (should be empty)
				if (!reader.isEndElement())
					reader.readElementText(QXmlStreamReader::IncludeChildElements);
				m_includes.push_back(include);
			}
			else if (name == QLatin1String("script"))
			{
				sawContent = true;
				// Script tags apply to the current plugin.
				const QString content = reader.readElementText(QXmlStreamReader::IncludeChildElements);
				if (!content.isEmpty())
				{
					if (!m_plugins.isEmpty())
					{
						Plugin &plugin = m_plugins.front();
						if (!plugin.script.isEmpty())
							plugin.script += "\n";
						plugin.script += content;
					}
					else
					{
						Script script;
						script.content = content;
						m_scripts.push_back(script);
					}
				}
			}
		}
	}

	if (reader.hasError())
	{
		m_errorString = reader.errorString();
		return false;
	}

	if (!sawContent)
	{
		m_errorString = QStringLiteral("File does not have a valid QMud XML signature.");
		return false;
	}

	if (m_plugins.size() > 1)
	{
		m_errorString = QStringLiteral("Can only have one plugin per file");
		return false;
	}

	if (!m_plugins.isEmpty())
	{
		Plugin       &plugin = m_plugins.front();
		const QString name   = plugin.attributes.value(QStringLiteral("name")).trimmed();
		if (name.isEmpty())
		{
			m_errorString = QStringLiteral("Plugin must have a name");
			return false;
		}

		QString nameCheck = name;
		nameCheck         = nameCheck.trimmed();
		if (nameCheck.isEmpty() || !nameCheck.at(0).isLetter())
		{
			m_errorString = QStringLiteral("Plugin name is invalid");
			return false;
		}
		for (int i = 1; i < nameCheck.size(); ++i)
		{
			const QChar ch = nameCheck.at(i);
			if (!ch.isLetterOrNumber() && ch != QLatin1Char('_'))
			{
				m_errorString = QStringLiteral("Plugin name is invalid");
				return false;
			}
		}

		if (name.size() > 32)
		{
			m_errorString = QStringLiteral("Plugin name cannot be > 32 characters");
			return false;
		}

		const QString author = plugin.attributes.value(QStringLiteral("author"));
		if (author.size() > 32)
		{
			m_errorString = QStringLiteral("Plugin author name cannot be > 32 characters");
			return false;
		}

		const QString purpose = plugin.attributes.value(QStringLiteral("purpose"));
		if (purpose.size() > 100)
		{
			m_errorString = QStringLiteral("Plugin purpose cannot be > 100 characters");
			return false;
		}

		QString pluginId = plugin.attributes.value(QStringLiteral("id")).trimmed();
		if (pluginId.isEmpty())
		{
			m_errorString = QStringLiteral("Plugin must have a unique \"id\" field");
			return false;
		}

		constexpr int kPluginIdLength = 24;
		if (pluginId.size() != kPluginIdLength)
		{
			m_errorString =
			    QStringLiteral("Plugin \"id\" field must be %1 characters long").arg(kPluginIdLength);
			return false;
		}

		for (const QChar ch : pluginId)
		{
			if (!ch.isDigit() && (ch.toLower() < QLatin1Char('a') || ch.toLower() > QLatin1Char('f')))
			{
				m_errorString =
				    QStringLiteral("Plugin \"id\" field must be %1 hex characters").arg(kPluginIdLength);
				return false;
			}
		}
		pluginId = pluginId.toLower();
		plugin.attributes.insert(QStringLiteral("id"), pluginId);

		QString language = plugin.attributes.value(QStringLiteral("language")).trimmed().toLower();
		plugin.attributes.insert(QStringLiteral("language"), language);
		if (!language.isEmpty() && language != QStringLiteral("lua"))
		{
			m_errorString =
			    QStringLiteral("Plugin language \"%1\" is not supported (Lua only)").arg(language);
			return false;
		}
		if (language.isEmpty() && !plugin.script.trimmed().isEmpty())
		{
			m_errorString = QStringLiteral("Plugin script supplied without a scripting language");
			return false;
		}

		const QString requiresText = plugin.attributes.value(QStringLiteral("requires")).trimmed();
		if (!requiresText.isEmpty())
		{
			QString actualText = QString::fromLatin1(kVersionString);
			actualText         = actualText.trimmed();
			int i              = 0;
			while (i < actualText.size() &&
			       (actualText.at(i).isDigit() || actualText.at(i) == QLatin1Char('.')))
				++i;
			actualText                   = actualText.left(i);
			bool         okActual        = false;
			const double actualVersion   = actualText.toDouble(&okActual);
			bool         okRequires      = false;
			const double requiresVersion = requiresText.toDouble(&okRequires);
			if (okActual && okRequires && requiresVersion > actualVersion)
			{
				m_errorString =
				    QStringLiteral("Plugin requires QMud version %1 or above").arg(requiresVersion);
				return false;
			}
		}
	}

	if (policy == PluginPolicy::ForbidPlugins && !m_plugins.isEmpty())
	{
		m_errorString = QStringLiteral("Plugin not expected here. Use File -> Plugins to load plugins");
		return false;
	}

	if (policy == PluginPolicy::RequirePlugin && m_plugins.isEmpty())
	{
		m_errorString = QStringLiteral("No plugin found");
		return false;
	}

	if (!includeContext && !(m_loadMask & XML_IMPORT_MAIN_FILE_ONLY))
	{
		if ((m_loadMask & XML_NO_PLUGINS) && !m_plugins.isEmpty())
		{
			m_errorString = QStringLiteral("Plugin not expected here. Use File -> Plugins to load plugins");
			return false;
		}
		if ((m_loadMask & XML_PLUGINS) && m_plugins.isEmpty() && m_pluginFile)
		{
			m_errorString = QStringLiteral("No plugin found");
			return false;
		}
	}

	if (!(m_loadMask & XML_GENERAL))
	{
		m_worldAttributes.clear();
		m_worldMultilineAttributes.clear();
	}
	if (!(m_loadMask & XML_TRIGGERS))
		m_triggers.clear();
	if (!(m_loadMask & XML_ALIASES))
		m_aliases.clear();
	if (!(m_loadMask & XML_TIMERS))
		m_timers.clear();
	if (!(m_loadMask & XML_MACROS))
		m_macros.clear();
	if (!(m_loadMask & XML_VARIABLES))
		m_variables.clear();
	if (!(m_loadMask & XML_COLOURS))
		m_colours.clear();
	if (!(m_loadMask & XML_KEYPAD))
		m_keypadEntries.clear();
	if (!(m_loadMask & XML_PRINTING))
		m_printingStyles.clear();
	if (!(m_loadMask & XML_INCLUDES))
		m_includes.clear();
	if (!(m_loadMask & XML_PLUGINS))
		m_plugins.clear();

	if (m_pluginFile && (!(m_loadMask & XML_INCLUDES) || m_includes.isEmpty()))
		finalizePluginContents();

	return true;
}

bool WorldDocument::expandIncludes(const QString &worldFilePath, const QString &pluginsDir,
                                   const QString &programDir, const QString &stateDir)
{
	if (m_loadMask & XML_IMPORT_MAIN_FILE_ONLY)
		return true;
	if (!(m_loadMask & XML_INCLUDES))
		return true;

	if (m_currentIncludeStack.isEmpty())
		m_currentIncludeStack.push_back(QFileInfo(worldFilePath).absoluteFilePath());

	if (m_loadedPluginIds.isEmpty())
	{
		for (const Plugin &plugin : m_plugins)
		{
			const QString pluginId   = plugin.attributes.value(QStringLiteral("id")).trimmed().toLower();
			const QString pluginName = plugin.attributes.value(QStringLiteral("name")).trimmed();
			if (!pluginId.isEmpty())
				m_loadedPluginIds.insert(pluginId, pluginName);
		}
	}

	if (!expandIncludesPass(worldFilePath, pluginsDir, programDir, stateDir, false, QString(), false))
		return false;
	if (!expandIncludesPass(worldFilePath, pluginsDir, programDir, stateDir, true, QString(), false))
		return false;
	finalizePluginContents();
	return true;
}

bool WorldDocument::expandIncludesPass(const QString &worldFilePath, const QString &pluginsDir,
                                       const QString &programDir, const QString &stateDir, bool wantPlugins,
                                       const QString &currentPluginDir, bool isIncludeContext)
{
	if (wantPlugins)
	{
		if (m_loadMask & XML_NO_PLUGINS)
			return true;
		if (!(m_loadMask & XML_PLUGINS))
			return true;
	}

	for (const Include &include : m_includes)
	{
		const QString rawName = include.attributes.value(QStringLiteral("name"));
		if (rawName.isEmpty())
		{
			m_errorString = QStringLiteral("Name of include file not specified");
			return false;
		}
		if (rawName.compare(QStringLiteral("clipboard"), Qt::CaseInsensitive) == 0)
		{
			m_errorString = QStringLiteral("Name of include file cannot be \"clipboard\"");
			return false;
		}

		const QString pluginFlag = include.attributes.value(QStringLiteral("plugin")).toLower();
		const bool    isPlugin   = (pluginFlag == QStringLiteral("y") || pluginFlag == QStringLiteral("1") ||
                               pluginFlag == QStringLiteral("true"));
		if (isPlugin != wantPlugins)
			continue;

		const QString resolved =
		    resolveIncludePath(rawName, worldFilePath, pluginsDir, programDir, currentPluginDir, wantPlugins);
		if (resolved.isEmpty())
			continue;

		// Non-plugin includes are tracked to prevent duplicate inclusion.
		if (!isPlugin && currentPluginDir.isEmpty())
		{
			if (m_currentIncludeStack.contains(resolved))
			{
				m_errorString = QStringLiteral("File \"%1\" has already been included.").arg(resolved);
				return false;
			}
			m_currentIncludeStack.push_back(resolved);
			if (!isIncludeContext)
				m_includeFileList.push_back(rawName);
		}

		WorldDocument child;
		child.setLoadMask(m_loadMask);
		if (!child.loadFromFileWithPolicy(
		        resolved, isPlugin ? PluginPolicy::RequirePlugin : PluginPolicy::ForbidPlugins, !isPlugin))
		{
			m_errorString = child.errorString();
			return false;
		}
		child.m_currentIncludeStack = m_currentIncludeStack;
		const QString childPluginDir = isPlugin ? QFileInfo(resolved).absolutePath() : currentPluginDir;
		if (!child.expandIncludesPass(resolved, pluginsDir, programDir, stateDir, false, childPluginDir,
		                              true))
		{
			m_errorString = child.errorString();
			return false;
		}
		if (!child.expandIncludesPass(resolved, pluginsDir, programDir, stateDir, true, childPluginDir, true))
		{
			m_errorString = child.errorString();
			return false;
		}
		child.finalizePluginContents();

		QString newPluginId;
		QString newPluginName;
		if (isPlugin && !child.m_plugins.isEmpty())
		{
			Plugin &childPlugin = child.m_plugins.front();
			childPlugin.attributes.insert(QStringLiteral("source"), resolved);
			childPlugin.attributes.insert(QStringLiteral("directory"), QFileInfo(resolved).absolutePath());
			newPluginId   = childPlugin.attributes.value(QStringLiteral("id")).trimmed().toLower();
			newPluginName = childPlugin.attributes.value(QStringLiteral("name")).trimmed();
			if (!newPluginId.isEmpty() && m_loadedPluginIds.contains(newPluginId))
			{
				const QString existingName = m_loadedPluginIds.value(newPluginId);
				m_errorString = QStringLiteral("The plugin '%1' is already loaded.").arg(existingName);
				return false;
			}
		}

		if (isPlugin)
		{
			const QString worldId = m_worldAttributes.value(QStringLiteral("id"));
			QString       pluginId;
			if (!child.m_plugins.isEmpty())
				pluginId = child.m_plugins.front().attributes.value(QStringLiteral("id"));

			if (!worldId.isEmpty() && !pluginId.isEmpty() && !stateDir.isEmpty())
			{
				QString base = stateDir;
				if (!base.endsWith('/') && !base.endsWith('\\'))
					base += '/';
				const QString stateFile = base + worldId + "-" + pluginId + "-state.xml";
				QFileInfo     info(stateFile);
				if (info.exists())
				{
					if (info.size() == 0)
					{
					}
					else if (!isArchiveXMLFile(info.absoluteFilePath()))
					{
						m_errorString = QStringLiteral("Plugin state \"%1\" is not an XML file")
						                    .arg(info.absoluteFilePath());
						return false;
					}
					else
					{
						WorldDocument stateDoc;
						if (!stateDoc.loadFromFileWithPolicy(info.absoluteFilePath(),
						                                     PluginPolicy::ForbidPlugins, false))
						{
							m_errorString = QStringLiteral("Error processing plugin state file \"%1\"")
							                    .arg(info.absoluteFilePath());
							return false;
						}
						if (!child.m_plugins.isEmpty())
							mergePluginVariablesFrom(child.m_plugins.front(), stateDoc.variables(), true);
					}
				}
			}
		}
		if (!mergeFrom(child, !isPlugin))
			return false;
		if (!newPluginId.isEmpty())
			m_loadedPluginIds.insert(newPluginId, newPluginName);
		if (!isPlugin && currentPluginDir.isEmpty() && !m_currentIncludeStack.isEmpty())
			m_currentIncludeStack.removeLast();
	}

	return true;
}

bool WorldDocument::isArchiveXMLFile(const QString &fileName)
{
	static const QStringList signatures = {
	    QStringLiteral("<?xml"),     QStringLiteral("<!--"),       QStringLiteral("<!DOCTYPE"),
	    QStringLiteral("<muclient"), QStringLiteral("<qmud"),      QStringLiteral("<world"),
	    QStringLiteral("<triggers"), QStringLiteral("<aliases"),   QStringLiteral("<timers"),
	    QStringLiteral("<macros"),   QStringLiteral("<variables"), QStringLiteral("<colours"),
	    QStringLiteral("<keypad"),   QStringLiteral("<printing"),  QStringLiteral("<comment"),
	    QStringLiteral("<include"),  QStringLiteral("<plugin"),    QStringLiteral("<script")};

	QFile file(fileName);
	if (!file.open(QIODevice::ReadOnly))
		return false;

	QByteArray buf = file.read(500);
	if (buf.isEmpty())
		return false;

	QString text;
	if (buf.size() >= 2 && static_cast<unsigned char>(buf[0]) == 0xFF &&
	    static_cast<unsigned char>(buf[1]) == 0xFE)
	{
		const QByteArray rest = buf.mid(2);
		const auto len  = rest.size() / 2;
		const auto *ptr = reinterpret_cast<const char16_t *>(rest.constData());
		text            = QString::fromUtf16(ptr, len);
	}
	else if (buf.size() >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
	         static_cast<unsigned char>(buf[1]) == 0xBB && static_cast<unsigned char>(buf[2]) == 0xBF)
	{
		text = QString::fromUtf8(buf.mid(3));
	}
	else
	{
		text = QString::fromUtf8(buf);
	}

	int pos = 0;
	while (pos < text.size() && text.at(pos).isSpace())
		++pos;

	text = text.mid(pos);
	if (text.size() < 15)
		return false;

	return std::ranges::any_of(signatures, [&text](const QString &sig) { return text.startsWith(sig); });
}

QString WorldDocument::resolveIncludePath(const QString &rawName, const QString &worldFilePath,
                                          const QString &pluginsDir, const QString &programDir,
                                          const QString &currentPluginDir, bool wantPlugins)
{
	QString name = rawName;
	if (name.isEmpty())
		return {};

	if (name.compare(QStringLiteral("clipboard"), Qt::CaseInsensitive) == 0)
		return {};

	QString worldDir = QFileInfo(worldFilePath).absolutePath();
	if (!worldDir.endsWith('/'))
		worldDir += '/';

	QString plugDir = pluginsDir;
	if (!plugDir.isEmpty() && !plugDir.endsWith('/') && !plugDir.endsWith('\\'))
		plugDir += '/';

	QString progDir = programDir;
	if (!progDir.isEmpty() && !progDir.endsWith('/'))
		progDir += '/';

	name.replace(QStringLiteral("$PLUGINSDEFAULTDIR"), plugDir);
	name.replace(QStringLiteral("$PROGRAMDIR"), progDir);
	name.replace(QStringLiteral("$WORLDDIR"), worldDir);
	if (!currentPluginDir.isEmpty())
	{
		QString cur = currentPluginDir;
		if (!cur.endsWith('/'))
			cur += '/';
		name.replace(QStringLiteral("$PLUGINDIR"), cur);
	}

	QString nativeName;
#ifdef Q_OS_WIN
	nativeName = QDir::fromNativeSeparators(name);
#else
	nativeName = name;
	nativeName.replace(QLatin1Char('\\'), QLatin1Char('/'));
#endif

	// Repair malformed "/C:/..." paths emitted by some legacy exports.
	if (nativeName.startsWith(QLatin1Char('/')) && nativeName.size() > 3 &&
	    nativeName.at(1).isLetter() && nativeName.at(2) == QLatin1Char(':'))
	{
		nativeName = nativeName.mid(1);
	}

	// Repair legacy malformed absolute-like portable paths (eg. "/worlds/...").
	if (nativeName.startsWith(QLatin1Char('/')))
	{
		QString candidate = nativeName;
		while (candidate.startsWith(QLatin1Char('/')))
			candidate.remove(0, 1);
		const QString lower = candidate.toLower();
		const bool    looksPortable =
		    lower == QStringLiteral("worlds") || lower.startsWith(QStringLiteral("worlds/")) ||
		    lower == QStringLiteral("logs") || lower.startsWith(QStringLiteral("logs/"));
		if (looksPortable)
			nativeName = candidate;
	}

	// Convert legacy absolute Windows paths that point into portable roots
	// (eg. "C:/Games/.../worlds/foo.xml") to portable-root paths.
	if (const bool isDrivePath = nativeName.size() >= 2 && nativeName.at(1) == QLatin1Char(':') &&
	                             nativeName.at(0).isLetter();
	    isDrivePath)
	{
		const QString lower = nativeName.toLower();
		qsizetype     pos   = lower.indexOf(QStringLiteral("/worlds/"));
		if (pos < 0 && (lower == QStringLiteral("worlds") || lower.endsWith(QStringLiteral("/worlds"))))
			pos = lower.lastIndexOf(QStringLiteral("/worlds"));
		if (pos < 0)
			pos = lower.indexOf(QStringLiteral("/logs/"));
		if (pos < 0 && (lower == QStringLiteral("logs") || lower.endsWith(QStringLiteral("/logs"))))
			pos = lower.lastIndexOf(QStringLiteral("/logs"));
		if (pos >= 0)
			nativeName = nativeName.mid(pos + 1);
	}

	const bool isDriveAbsolute =
	    (nativeName.size() >= 2 && nativeName.at(1) == QLatin1Char(':') && nativeName.at(0).isLetter());
	const bool isUNC =
	    nativeName.startsWith(QStringLiteral("//")) || nativeName.startsWith(QStringLiteral("\\\\"));
	if (QDir::isAbsolutePath(nativeName))
		return QDir::cleanPath(nativeName);
	if (isDriveAbsolute || isUNC)
	{
#ifdef Q_OS_WIN
		return QDir::cleanPath(QFileInfo(nativeName).absoluteFilePath());
#else
		return QDir::cleanPath(nativeName);
#endif
	}

	const bool isExplicitRelative =
	    nativeName.startsWith(QStringLiteral("./")) || nativeName.startsWith(QStringLiteral(".\\")) ||
	    nativeName.startsWith(QStringLiteral("../")) || nativeName.startsWith(QStringLiteral("..\\"));

	const QString normalizedLower    = nativeName.toLower();
	const bool    startsPortableRoot = normalizedLower == QStringLiteral("worlds") ||
	                                normalizedLower.startsWith(QStringLiteral("worlds/")) ||
	                                normalizedLower == QStringLiteral("logs") ||
	                                normalizedLower.startsWith(QStringLiteral("logs/"));

	if (isExplicitRelative)
	{
		const QString base = (wantPlugins || !currentPluginDir.isEmpty())
		                         ? (plugDir.isEmpty() ? worldDir : plugDir)
		                         : worldDir;
		return QDir::cleanPath(QDir(base).filePath(nativeName));
	}

	if (startsPortableRoot)
	{
		const QString base = progDir.isEmpty() ? worldDir : progDir;
		return QDir::cleanPath(QDir(base).filePath(nativeName));
	}

	// Relative path: prefer plugin dir when set, otherwise world dir.
	if (wantPlugins || !currentPluginDir.isEmpty())
	{
		const QString base = plugDir.isEmpty() ? worldDir : plugDir;
		return QDir::cleanPath(QDir(base).filePath(nativeName));
	}
	return QDir::cleanPath(QDir(worldDir).filePath(nativeName));
}

bool WorldDocument::mergeFrom(const WorldDocument &other, bool fromInclude)
{
	const unsigned int flags = fromInclude ? m_includeMergeFlags : 0;

	auto               warn = [this, flags](const QString &message)
	{
		if (flags & IncludeMergeWarn)
			m_warnings.push_back(message);
	};

	for (auto it = other.m_worldAttributes.begin(); it != other.m_worldAttributes.end(); ++it)
	{
		if (m_worldAttributes.contains(it.key()))
		{
			if (flags & IncludeMergeKeep)
			{
				warn(QStringLiteral("Duplicate world attribute \"%1\" ignored").arg(it.key()));
				continue;
			}
			if (!(flags & IncludeMergeOverwrite))
			{
				m_errorString = QStringLiteral("Duplicate world attribute \"%1\" ").arg(it.key());
				return false;
			}
			warn(QStringLiteral("Duplicate world attribute \"%1\" overwritten").arg(it.key()));
		}
		m_worldAttributes.insert(it.key(), it.value());
	}
	for (auto it = other.m_worldMultilineAttributes.begin(); it != other.m_worldMultilineAttributes.end();
	     ++it)
	{
		if (m_worldMultilineAttributes.contains(it.key()))
		{
			if (flags & IncludeMergeKeep)
			{
				warn(QStringLiteral("Duplicate world attribute \"%1\" ignored").arg(it.key()));
				continue;
			}
			if (!(flags & IncludeMergeOverwrite))
			{
				m_errorString = QStringLiteral("Duplicate world attribute \"%1\" ").arg(it.key());
				return false;
			}
			warn(QStringLiteral("Duplicate world attribute \"%1\" overwritten").arg(it.key()));
		}
		m_worldMultilineAttributes.insert(it.key(), it.value());
	}

	auto mergeNamedList = [&](auto &dest, const auto &src, const QString &kind, bool defaultOverwrite)
	{
		QMap<QString, int> indexByName;
		for (int i = 0; i < dest.size(); ++i)
		{
			const QString name = dest[i].attributes.value(QStringLiteral("name")).trimmed().toLower();
			if (!name.isEmpty())
				indexByName.insert(name, i);
		}

		for (const auto &item : src)
		{
			const QString rawName = item.attributes.value(QStringLiteral("name")).trimmed();
			const QString key     = rawName.toLower();
			if (key.isEmpty() || !indexByName.contains(key))
			{
				dest.push_back(item);
				if (!key.isEmpty())
					indexByName.insert(key, dest.size() - 1);
				continue;
			}

			const bool keep      = (flags & IncludeMergeKeep) != 0;
			const bool overwrite = (flags & IncludeMergeOverwrite) != 0 || defaultOverwrite;
			if (keep && !overwrite)
			{
				warn(QStringLiteral("Duplicate %1 label \"%2\" ignored").arg(kind, rawName));
				continue;
			}
			if (!overwrite)
			{
				m_errorString = QStringLiteral("Duplicate %1 label \"%2\" ").arg(kind, rawName);
				return false;
			}
			warn(QStringLiteral("Duplicate %1 label \"%2\" overwritten").arg(kind, rawName));
			dest[indexByName.value(key)] = item;
		}
		return true;
	};

	if (!mergeNamedList(m_triggers, other.m_triggers, QStringLiteral("trigger"), false))
		return false;
	if (!mergeNamedList(m_aliases, other.m_aliases, QStringLiteral("alias"), false))
		return false;
	if (!mergeNamedList(m_timers, other.m_timers, QStringLiteral("timer"), false))
		return false;
	if (!mergeNamedList(m_macros, other.m_macros, QStringLiteral("macro"), true))
		return false;

	auto mergeVariables = [&](auto &dest, const auto &src)
	{
		QMap<QString, int> indexByName;
		for (int i = 0; i < dest.size(); ++i)
		{
			const QString name = dest[i].attributes.value(QStringLiteral("name")).trimmed().toLower();
			if (!name.isEmpty())
				indexByName.insert(name, i);
		}

		for (const auto &item : src)
		{
			const QString rawName = item.attributes.value(QStringLiteral("name")).trimmed();
			const QString key     = rawName.toLower();
			if (key.isEmpty() || !indexByName.contains(key))
			{
				dest.push_back(item);
				if (!key.isEmpty())
					indexByName.insert(key, dest.size() - 1);
				continue;
			}

			if ((flags & IncludeMergeKeep) != 0)
			{
				warn(QStringLiteral("%1: duplicate variable ignored").arg(rawName));
				continue;
			}
			if ((flags & IncludeMergeWarn) != 0 && dest[indexByName.value(key)].content != item.content)
				warn(QStringLiteral("%1: overwriting existing variable contents").arg(rawName));
			dest[indexByName.value(key)] = item;
		}
		return true;
	};

	if (!mergeVariables(m_variables, other.m_variables))
		return false;

	m_colours.append(other.m_colours);
	m_keypadEntries.append(other.m_keypadEntries);
	m_printingStyles.append(other.m_printingStyles);
	m_plugins.append(other.m_plugins);
	m_includes.append(other.m_includes);
	m_scripts.append(other.m_scripts);
	if (!other.m_comments.isEmpty())
	{
		if (!m_comments.isEmpty())
			m_comments += "\n";
		m_comments += other.m_comments;
	}
	return true;
}

void WorldDocument::mergeVariablesFrom(const WorldDocument &other, bool overwrite)
{
	if (!overwrite)
	{
		m_variables.append(other.m_variables);
		return;
	}

	QMap<QString, qsizetype> indexByName;
	for (qsizetype i = 0; i < m_variables.size(); ++i)
	{
		const QString name = m_variables[i].attributes.value(QStringLiteral("name"));
		if (!name.isEmpty())
			indexByName.insert(name, i);
	}

	for (const auto &v : other.m_variables)
	{
		const QString name = v.attributes.value(QStringLiteral("name"));
		if (!name.isEmpty() && indexByName.contains(name))
			m_variables[indexByName.value(name)] = v;
		else
		{
			m_variables.append(v);
			if (!name.isEmpty())
				indexByName.insert(name, m_variables.size() - 1);
		}
	}
}

void WorldDocument::mergePluginVariablesFrom(Plugin &plugin, const QList<Variable> &vars, bool overwrite)
{
	if (!overwrite)
	{
		plugin.variables.append(vars);
		return;
	}

	QMap<QString, qsizetype> indexByName;
	for (qsizetype i = 0; i < plugin.variables.size(); ++i)
	{
		const QString name = plugin.variables[i].attributes.value(QStringLiteral("name")).trimmed().toLower();
		if (!name.isEmpty())
			indexByName.insert(name, i);
	}

	for (const auto &v : vars)
	{
		const QString name = v.attributes.value(QStringLiteral("name")).trimmed();
		const QString key  = name.toLower();
		if (!key.isEmpty() && indexByName.contains(key))
		{
			plugin.variables[indexByName.value(key)] = v;
		}
		else
		{
			plugin.variables.append(v);
			if (!key.isEmpty())
				indexByName.insert(key, plugin.variables.size() - 1);
		}
	}
}

void WorldDocument::finalizePluginContents()
{
	if (!m_pluginFile || m_pluginContentFinalized)
		return;
	if (m_plugins.isEmpty())
		return;

	Plugin &plugin = m_plugins.front();
	if (!m_triggers.isEmpty())
		plugin.triggers = m_triggers;
	if (!m_aliases.isEmpty())
		plugin.aliases = m_aliases;
	if (!m_timers.isEmpty())
		plugin.timers = m_timers;
	if (!m_variables.isEmpty())
		plugin.variables = m_variables;

	m_triggers.clear();
	m_aliases.clear();
	m_timers.clear();
	m_variables.clear();
	m_pluginContentFinalized = true;
}

QString WorldDocument::errorString() const
{
	return m_errorString;
}

int WorldDocument::worldFileVersion() const
{
	return m_worldFileVersion;
}

QString WorldDocument::qmudVersion() const
{
	return m_qmudVersion;
}

QDateTime WorldDocument::dateSaved() const
{
	return m_dateSaved;
}

const QMap<QString, QString> &WorldDocument::worldAttributes() const
{
	return m_worldAttributes;
}

const QMap<QString, QString> &WorldDocument::worldMultilineAttributes() const
{
	return m_worldMultilineAttributes;
}

const QList<WorldDocument::Trigger> &WorldDocument::triggers() const
{
	return m_triggers;
}

const QList<WorldDocument::Alias> &WorldDocument::aliases() const
{
	return m_aliases;
}

const QList<WorldDocument::Timer> &WorldDocument::timers() const
{
	return m_timers;
}

const QList<WorldDocument::Macro> &WorldDocument::macros() const
{
	return m_macros;
}

const QList<WorldDocument::Variable> &WorldDocument::variables() const
{
	return m_variables;
}

const QList<WorldDocument::Colour> &WorldDocument::colours() const
{
	return m_colours;
}

const QList<WorldDocument::Keypad> &WorldDocument::keypadEntries() const
{
	return m_keypadEntries;
}

const QList<WorldDocument::PrintingStyle> &WorldDocument::printingStyles() const
{
	return m_printingStyles;
}

QString WorldDocument::comments() const
{
	return m_comments;
}

const QList<WorldDocument::Plugin> &WorldDocument::plugins() const
{
	return m_plugins;
}

const QList<WorldDocument::Include> &WorldDocument::includes() const
{
	return m_includes;
}

const QList<WorldDocument::Script> &WorldDocument::scripts() const
{
	return m_scripts;
}

const QStringList &WorldDocument::includeFileList() const
{
	return m_includeFileList;
}

void WorldDocument::setLoadMask(unsigned long mask)
{
	m_loadMask = mask;
}

unsigned long WorldDocument::loadMask() const
{
	return m_loadMask;
}

void WorldDocument::setIncludeMergeFlags(unsigned int flags)
{
	m_includeMergeFlags = flags;
}

unsigned int WorldDocument::includeMergeFlags() const
{
	return m_includeMergeFlags;
}

const QStringList &WorldDocument::warnings() const
{
	return m_warnings;
}
