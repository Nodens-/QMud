/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: Environment.cpp
 * Role: Parses and resolves QMud-scoped environment variables from process environment with optional system-config fallback.
 */

#include "Environment.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
// ReSharper disable once CppUnusedIncludeDirective
#include <QStringList>

namespace
{
	QString stripOptionalQuotes(const QString &value)
	{
		if (value.size() < 2)
			return value;

		const QChar first = value.front();
		const QChar last  = value.back();
		if ((first == QLatin1Char('"') && last == QLatin1Char('"')) ||
		    (first == QLatin1Char('\'') && last == QLatin1Char('\'')))
		{
			return value.mid(1, value.size() - 2);
		}

		return value;
	}

	QString expandLeadingTilde(const QString &value)
	{
		if (value.isEmpty() || value.front() != QLatin1Char('~'))
			return value;
		if (value.size() == 1)
			return QDir::homePath();
#ifdef Q_OS_WIN
		if (value.at(1) == QLatin1Char('/') || value.at(1) == QLatin1Char('\\'))
#else
		if (value.at(1) == QLatin1Char('/'))
#endif
			return QDir::homePath() + value.mid(1);
		return value;
	}

	QStringList qmudConfigSearchPaths()
	{
#if defined(Q_OS_MACOS)
		return {QDir::home().filePath(QStringLiteral("Library/Application Support/QMud/config")),
		        QStringLiteral("/Library/Application Support/QMud/config")};
#elif defined(Q_OS_LINUX)
		return {QDir::home().filePath(QStringLiteral(".config/QMud/config")),
		        QStringLiteral("/etc/QMud/config")};
#elif defined(Q_OS_WIN)
		QString localAppData = qEnvironmentVariable("LOCALAPPDATA").trimmed();
		if (localAppData.isEmpty())
			localAppData = QDir::home().filePath(QStringLiteral("AppData/Local"));
		return {QDir(localAppData).filePath(QStringLiteral("QMud/config"))};
#else
		return {};
#endif
	}

	bool isQmudScopedVariable(const QString &name)
	{
		return name.startsWith(QStringLiteral("QMUD_"), Qt::CaseInsensitive);
	}

	QString normalizedVariableName(const QString &name)
	{
		return name.trimmed().toUpper();
	}

	bool &configFallbackEnabledFlag()
	{
		static bool enabled = true;
		return enabled;
	}

	bool &configSearchPathsOverrideEnabledFlag()
	{
		static bool enabled = false;
		return enabled;
	}

	QStringList &configSearchPathsOverride()
	{
		static QStringList paths;
		return paths;
	}

	QHash<QString, QString> &parsedConfigCache()
	{
		static QHash<QString, QString> cache;
		return cache;
	}

	bool &parsedConfigCacheLoaded()
	{
		static bool loaded = false;
		return loaded;
	}

	void resetParsedConfigCache()
	{
		parsedConfigCache().clear();
		parsedConfigCacheLoaded() = false;
	}

	const QStringList &resolvedConfigSearchPaths()
	{
		if (configSearchPathsOverrideEnabledFlag())
			return configSearchPathsOverride();
		static const QStringList defaultPaths = qmudConfigSearchPaths();
		return defaultPaths;
	}

	const QHash<QString, QString> &qmudVariablesFromSystemConfig()
	{
		if (!parsedConfigCacheLoaded())
		{
			QHash<QString, QString> parsedVariables;

			for (const QString &configPath : resolvedConfigSearchPaths())
			{
				if (configPath.isEmpty() || !QFileInfo::exists(configPath))
					continue;

				QFile configFile(configPath);
				if (!configFile.open(QIODevice::ReadOnly | QIODevice::Text))
				{
					qWarning() << "Failed to read QMud config:" << configPath;
					continue;
				}

				const QString content = QString::fromUtf8(configFile.readAll());
				for (const QStringList lines = content.split(QLatin1Char('\n')); QString line : lines)
				{
					line = line.trimmed();
					if (line.isEmpty() || line.startsWith(QLatin1Char('#')) ||
					    line.startsWith(QLatin1Char(';')))
						continue;
					if (line.startsWith(QStringLiteral("export ")))
						line = line.mid(7).trimmed();

					const qsizetype equalsPos = line.indexOf(QLatin1Char('='));
					if (equalsPos <= 0)
						continue;

					const QString key = normalizedVariableName(line.left(equalsPos));
					if (key.isEmpty() || !isQmudScopedVariable(key) || parsedVariables.contains(key))
						continue;

					QString value = stripOptionalQuotes(line.mid(equalsPos + 1).trimmed());
					if (key == QStringLiteral("QMUD_HOME"))
						value = expandLeadingTilde(value.trimmed());
					parsedVariables.insert(key, value);
				}
			}

			parsedConfigCache()       = parsedVariables;
			parsedConfigCacheLoaded() = true;
		}

		return parsedConfigCache();
	}
} // namespace

QString qmudEnvironmentVariable(const QString &name)
{
	const QString trimmedName = name.trimmed();
	if (trimmedName.isEmpty())
		return {};

	const QByteArray utf8Name = trimmedName.toUtf8();
	if (qEnvironmentVariableIsSet(utf8Name.constData()))
	{
		const QString environmentValue = qEnvironmentVariable(utf8Name.constData());
		if (!isQmudScopedVariable(trimmedName))
			return environmentValue;
		if (!configFallbackEnabledFlag())
			return environmentValue;
		if (!environmentValue.trimmed().isEmpty())
			return environmentValue;
		if (normalizedVariableName(trimmedName) != QStringLiteral("QMUD_HOME"))
			return environmentValue;
	}
	if (!configFallbackEnabledFlag())
		return {};
	if (!isQmudScopedVariable(trimmedName))
		return {};

	return qmudVariablesFromSystemConfig().value(normalizedVariableName(trimmedName));
}

bool qmudEnvironmentVariableIsSet(const QString &name)
{
	const QString trimmedName = name.trimmed();
	if (trimmedName.isEmpty())
		return false;

	const QByteArray utf8Name = trimmedName.toUtf8();
	if (qEnvironmentVariableIsSet(utf8Name.constData()))
		return true;
	if (!configFallbackEnabledFlag())
		return false;
	if (!isQmudScopedVariable(trimmedName))
		return false;

	return qmudVariablesFromSystemConfig().contains(normalizedVariableName(trimmedName));
}

bool qmudEnvironmentVariableIsEmpty(const QString &name)
{
	return qmudEnvironmentVariable(name).isEmpty();
}

void qmudSetEnvironmentConfigFallbackEnabled(const bool enabled)
{
	configFallbackEnabledFlag() = enabled;
}

void qmudSetEnvironmentConfigSearchPathsForTesting(const QStringList &paths)
{
	configSearchPathsOverrideEnabledFlag() = true;
	configSearchPathsOverride()            = paths;
	resetParsedConfigCache();
}

void qmudClearEnvironmentConfigSearchPathsForTesting()
{
	configSearchPathsOverrideEnabledFlag() = false;
	configSearchPathsOverride().clear();
	resetParsedConfigCache();
}
