/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TestEnvironment.h
 * Role: Deterministic test-environment declarations and temporary-directory helpers shared by QTest targets.
 */

#ifndef QMUD_TESTENVIRONMENT_H
#define QMUD_TESTENVIRONMENT_H

#include <QTemporaryDir>

namespace QMudTest
{
	/**
	 * @brief Applies deterministic locale/timezone process settings for tests.
	 */
	void applyDeterministicTestEnvironment();

	/**
	 * @brief RAII temporary-directory wrapper used by test fixtures.
	 */
	class ScopedTempDir
	{
		public:
			/**
			 * @brief Creates unique temporary directory handle.
			 */
			ScopedTempDir();

			/**
			 * @brief Reports whether temporary directory is valid.
			 * @return `true` when directory handle is valid.
			 */
			[[nodiscard]] bool    isValid() const;

			/**
			 * @brief Returns temporary directory absolute path.
			 * @return Filesystem path of managed temp directory.
			 */
			[[nodiscard]] QString path() const;

		private:
			QTemporaryDir m_dir;
	};
} // namespace QMudTest

#endif // QMUD_TESTENVIRONMENT_H
