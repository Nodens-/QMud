/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TestEnvironment.cpp
 * Role: Test startup configuration, deterministic environment, enforced silent sound output, and
 * temporary-directory helpers.
 */

#include "TestEnvironment.h"
#include "SoundEngine.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>

namespace
{
	/** @brief Disables FFmpeg video hardware probing before test applications and worker threads start. */
	void configureTestMultimedia()
	{
		qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", QByteArrayLiteral(","));
		qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", QByteArrayLiteral(","));
	}

	Q_CONSTRUCTOR_FUNCTION(configureTestMultimedia)
} // namespace

bool SoundEngine::isOutputMuted() noexcept
{
	return true;
}

namespace QMudTest
{
	void applyDeterministicTestEnvironment()
	{
		qputenv("TZ", QByteArrayLiteral("UTC"));
		qputenv("LANG", QByteArrayLiteral("C.UTF-8"));
		qputenv("LC_ALL", QByteArrayLiteral("C.UTF-8"));
	}

	ScopedTempDir::ScopedTempDir()
	{
		if (m_dir.isValid())
		{
			if (!QDir().mkpath(m_dir.path()))
				qWarning() << "Failed to create temporary test directory:" << m_dir.path();
		}
	}

	bool ScopedTempDir::isValid() const
	{
		return m_dir.isValid();
	}

	QString ScopedTempDir::path() const
	{
		return m_dir.path();
	}
} // namespace QMudTest
