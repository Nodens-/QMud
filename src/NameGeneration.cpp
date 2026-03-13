/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: NameGeneration.cpp
 * Role: Name-generation algorithms and helper logic used by UI workflows that suggest or synthesize names.
 */

#include "NameGeneration.h"

#include "AppController.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTextStream>

#include <stdexcept>

namespace
{
	NameGenerator *resolveGenerator();

	NameGenerator *resolveGenerator()
	{
		if (AppController *app = AppController::instance())
		{
			if (NameGenerator *generator = app->nameGenerator())
				return generator;
		}
		static NameGenerator fallback;
		return &fallback;
	}
} // namespace

NameGenerator::NameGenerator(AppController *app) : m_app(app)
{
}

void NameGenerator::setAppController(AppController *app)
{
	m_app = app;
}

bool NameGenerator::readNextNameLine(QTextStream &stream, QString &line)
{
	while (!stream.atEnd())
	{
		line = stream.readLine().trimmed();
		if (line.startsWith(QStringLiteral("/*")))
			continue;
		return true;
	}
	return false;
}

bool NameGenerator::matchesTag(const QString &line, const char *tag)
{
	return QString::compare(line, QString::fromUtf8(tag), Qt::CaseInsensitive) == 0;
}

QString NameGenerator::resolveNamesFilePath(const QString &fileNameInput, const bool noDialog) const
{
	QString    fileName     = fileNameInput;
	const bool forcedReload = (fileNameInput == QStringLiteral("*"));
	const bool needsDefault = fileName.isEmpty() || forcedReload;
	if (needsDefault && m_app)
		fileName = m_app->getGlobalOption(QStringLiteral("DefaultNameGenerationFile")).toString();

	bool foundFile = false;
	if (!forcedReload && !fileName.isEmpty())
		foundFile = QFileInfo::exists(fileName);

	// Compatibility + new canonical location:
	// if configured/default points to legacy datadir root names.txt, prefer names/names.txt.
	if (!foundFile && !fileName.isEmpty())
	{
		const QString normalized = fileName.toLower().replace(QLatin1Char('\\'), QLatin1Char('/'));
		if (normalized == QStringLiteral("names.txt") || normalized == QStringLiteral("./names.txt"))
		{
			const QString canonical = QStringLiteral("names/names.txt");
			if (QFileInfo::exists(canonical))
			{
				fileName  = canonical;
				foundFile = true;
				if (m_app)
					m_app->setGlobalOptionString(QStringLiteral("DefaultNameGenerationFile"), fileName);
			}
		}
	}

	if (!foundFile && !noDialog)
	{
		if (m_app)
			m_app->changeToFileBrowsingDirectory();
		const QString chosen =
		    QFileDialog::getOpenFileName(nullptr, QStringLiteral("Select names file"), fileName,
		                                 QStringLiteral("Name files (*.txt *.nam);;All files (*.*)"));
		if (m_app)
			m_app->changeToStartupDirectory();
		if (chosen.isEmpty())
			return {};
		fileName = chosen;
		if (m_app)
			m_app->setGlobalOptionString(QStringLiteral("DefaultNameGenerationFile"), fileName);
	}

	return fileName;
}

void NameGenerator::readNames(const QString &fileName, const bool noDialog)
{
	m_namesRead = false;
	m_firstNames.clear();
	m_middleNames.clear();
	m_lastNames.clear();

	const QString resolvedName = resolveNamesFilePath(fileName, noDialog);
	if (resolvedName.isEmpty())
		return;

	QFile namesFile(resolvedName);
	if (!namesFile.open(QIODevice::ReadOnly | QIODevice::Text))
		throw std::runtime_error(
		    QStringLiteral("Could not read names file \"%1\"").arg(resolvedName).toStdString());

	QTextStream stream(&namesFile);
	QString     line;
	bool        ok = false;

	while ((ok = readNextNameLine(stream, line)))
	{
		if (matchesTag(line, "[start]") || matchesTag(line, "[startstav]"))
			break;
	}
	if (!ok)
		throw std::runtime_error("No [start] in names file");

	while ((ok = readNextNameLine(stream, line)))
	{
		if (matchesTag(line, "[middle]") || matchesTag(line, "[mittstav]"))
			break;
		m_firstNames.append(line);
	}
	if (!ok)
		throw std::runtime_error("No [middle] in names file");

	while ((ok = readNextNameLine(stream, line)))
	{
		if (matchesTag(line, "[end]") || matchesTag(line, "[slutstav]"))
			break;
		m_middleNames.append(line);
	}
	if (!ok)
		throw std::runtime_error("No [end] in names file");

	while ((ok = readNextNameLine(stream, line)))
	{
		if (matchesTag(line, "[stop]"))
			break;
		m_lastNames.append(line);
	}
	if (!ok)
		throw std::runtime_error("No [stop] in names file");

	m_namesRead = true;
}

QString NameGenerator::generateName()
{
	try
	{
		if (!m_namesRead)
			readNames(QString(), false);
	}
	catch (const std::exception &)
	{
		return {};
	}

	if (!m_namesRead || m_firstNames.isEmpty() || m_middleNames.isEmpty() || m_lastNames.isEmpty())
		return {};

	const auto first  = QRandomGenerator::global()->bounded(m_firstNames.size());
	const auto middle = QRandomGenerator::global()->bounded(m_middleNames.size());
	const auto last   = QRandomGenerator::global()->bounded(m_lastNames.size());
	return m_firstNames.at(first) + m_middleNames.at(middle) + m_lastNames.at(last);
}

bool NameGenerator::isLoaded() const
{
	return m_namesRead;
}

void qmudReadNames(const QString &fileName, const bool noDialog)
{
	resolveGenerator()->readNames(fileName, noDialog);
}

QString qmudGenerateName()
{
	return resolveGenerator()->generateName();
}
