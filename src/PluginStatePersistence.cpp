/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: PluginStatePersistence.cpp
 * Role: Dedicated plugin-state XML writer with per-file minute throttling and latest-state coalescing.
 */
#include "PluginStatePersistence.h"
#include "Version.h"
#include "helpers/XmlEscapeUtils.h"
#include <QLocale>
#include <QSaveFile>
#include <QTextStream>
#include <algorithm>
#include <cstdio>
#include <exception>
#include <utility>

PluginStatePersistence::PluginStatePersistence(Clock clock, Writer writer)
    : m_clock(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })),
      m_writer(writer ? std::move(writer) : Writer(writeSnapshot))
{
}

PluginStatePersistence::~PluginStatePersistence()
{
	flush();
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_wake.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}

PluginStatePersistence &PluginStatePersistence::instance()
{
	static PluginStatePersistence service;
	return service;
}

quint64 PluginStatePersistence::reserveRevision()
{
	std::lock_guard lock(m_mutex);
	return m_nextRevision++;
}

bool PluginStatePersistence::submit(Snapshot snapshot, const bool immediate)
{
	if (snapshot.targetFilePath.isEmpty())
		return false;
	std::optional<Snapshot> replaced;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping)
			return false;
		if (snapshot.revision == 0)
			snapshot.revision = m_nextRevision++;
		auto &destination = m_destinations[snapshot.targetFilePath];
		if (snapshot.revision > destination.latestRevision)
		{
			destination.latestRevision = snapshot.revision;
			if (destination.pending)
				replaced = std::move(destination.pending);
			destination.pending = std::move(snapshot);
		}
		if (immediate)
			destination.forcedRevision = destination.latestRevision;
		if (!m_worker.joinable())
			m_worker = std::thread([this] { run(); });
	}
	m_wake.notify_all();
	replaced.reset();
	return true;
}

void PluginStatePersistence::flush(const QString &targetFilePath)
{
	std::unique_lock        lock(m_mutex);
	QHash<QString, quint64> goals;
	for (auto it = m_destinations.begin(); it != m_destinations.end(); ++it)
	{
		if (!targetFilePath.isEmpty() && it.key() != targetFilePath)
			continue;
		goals.insert(it.key(), it->latestRevision);
		it->forcedRevision = std::max(it->forcedRevision, it->latestRevision);
	}
	m_wake.notify_all();
	m_wake.wait(lock,
	            [this, &goals]
	            {
		            for (auto it = goals.cbegin(); it != goals.cend(); ++it)
			            if (m_destinations.constFind(it.key())->completedRevision < it.value())
				            return false;
		            return true;
	            });
}

void PluginStatePersistence::run()
{
	std::unique_lock lock(m_mutex);
	for (;;)
	{
		const auto now    = m_clock();
		auto       chosen = m_destinations.end();
		auto       next   = TimePoint::max();
		for (auto it = m_destinations.begin(); it != m_destinations.end(); ++it)
		{
			if (!it->pending)
				continue;
			const auto due = it->forcedRevision > it->completedRevision || !it->lastStarted
			                     ? now
			                     : *it->lastStarted + std::chrono::minutes(1);
			if (due <= now &&
			    (chosen == m_destinations.end() || it->pending->revision < chosen->pending->revision))
				chosen = it;
			next = std::min(next, due);
		}
		if (chosen == m_destinations.end())
		{
			if (m_stopping)
				return;
			if (next == TimePoint::max())
				m_wake.wait(lock);
			else
				m_wake.wait_for(lock, next - now);
			continue;
		}
		Snapshot      snapshot = std::move(*chosen->pending);
		const QString path     = snapshot.targetFilePath;
		const quint64 revision = snapshot.revision;
		chosen->pending.reset();
		chosen->lastStarted = m_clock();
		lock.unlock();
		QString error;
		bool    written = false;
		try
		{
			written = m_writer(snapshot, error);
		}
		catch (const std::exception &exception)
		{
			error = QString::fromUtf8(exception.what());
		}
		catch (...)
		{
			error = QStringLiteral("Unknown file writer failure");
		}
		if (!written)
			std::fprintf(stderr, "[QMud] Unable to save plugin state '%s': %s\n",
			             qUtf8Printable(snapshot.targetFilePath), qUtf8Printable(error));
		snapshot = {};
		lock.lock();
		auto &destination             = m_destinations[path];
		destination.completedRevision = std::max(destination.completedRevision, revision);
		m_wake.notify_all();
	}
}

bool PluginStatePersistence::writeSnapshot(const Snapshot &snapshot, QString &error)
{
	using namespace QMudXmlEscapeUtils;
	QSaveFile file(snapshot.targetFilePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		error = file.errorString();
		return false;
	}
	QTextStream out(&file);
	out.setEncoding(QStringConverter::Utf8);
	const auto nl = QStringLiteral("\r\n");
	const auto savedOn =
	    QLocale::system().toString(snapshot.savedAt, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
	out << QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>)") << nl;
	out << QStringLiteral("<!DOCTYPE qmud>") << nl;
	out << QStringLiteral("<!-- Saved on ") << escapeXml(savedOn, XmlEscapeContext::Attribute)
	    << QStringLiteral(" -->") << nl;
	out << QStringLiteral("<!-- QMud version ") << kVersionString << QStringLiteral(" -->") << nl;
	out << QStringLiteral("<!-- Written by Panagiotis Kalogiratos -->") << nl;
	out << QStringLiteral("<!-- Home Page: https://github.com/Nodens-/QMud -->") << nl;
	out << QStringLiteral("<qmud>") << nl << nl << QStringLiteral("<!-- variables -->") << nl;
	out << QStringLiteral("<variables>") << nl;
	snapshot.variables.forEachVariable(
	    [&out, &nl](const QString &label, const QString &value)
	    {
		    out << QStringLiteral("  <variable name=\"") << escapeXml(label, XmlEscapeContext::Attribute)
		        << QStringLiteral("\">") << escapeXml(value, XmlEscapeContext::Text)
		        << QStringLiteral("</variable>") << nl;
	    });
	out << QStringLiteral("</variables>") << nl << QStringLiteral("</qmud>") << nl;
	out.flush();
	if (out.status() != QTextStream::Ok || !file.commit())
	{
		error = file.errorString();
		return false;
	}
	return true;
}
