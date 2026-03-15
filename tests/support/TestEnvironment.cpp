/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TestEnvironment.cpp
 * Role: Deterministic test-environment implementation and temporary-directory helpers shared by QTest targets.
 */

#include "TestEnvironment.h"

#include <QDebug>
#include <QDir>

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
