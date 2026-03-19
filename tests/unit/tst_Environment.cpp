/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_Environment.cpp
 * Role: Unit coverage for QMud environment/config fallback resolution behavior.
 */

#include "Environment.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

namespace
{
	class ScopedConfigSearchOverride
	{
		public:
			ScopedConfigSearchOverride(const QString &configFilePath, const QString &configText)
			{
				QFile file(configFilePath);
				QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate));
				QVERIFY(file.write(configText.toUtf8()) >= 0);
				file.close();

				qmudSetEnvironmentConfigSearchPathsForTesting(QStringList{configFilePath});
			}

			~ScopedConfigSearchOverride()
			{
				qmudClearEnvironmentConfigSearchPathsForTesting();
			}
	};

	class ScopedEnvVar
	{
		public:
				ScopedEnvVar(QByteArray name, const QByteArray &value) : m_name(std::move(name))
			{
				m_hadOriginal = qEnvironmentVariableIsSet(m_name.constData());
				if (m_hadOriginal)
					m_originalValue = qgetenv(m_name.constData());
				qputenv(m_name.constData(), value);
			}

			~ScopedEnvVar()
			{
				if (m_hadOriginal)
					qputenv(m_name.constData(), m_originalValue);
				else
					qunsetenv(m_name.constData());
			}

		private:
			QByteArray m_name;
			QByteArray m_originalValue;
			bool       m_hadOriginal{false};
	};
} // namespace

/**
 * @brief QTest fixture for Environment fallback and multi-instance safety behavior.
 */
class tst_Environment : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void emptyQmudHomeUsesConfigFallbackWhenEnabled()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString                    configPath = tempDir.filePath(QStringLiteral("config"));
			const ScopedConfigSearchOverride configOverride(
			    configPath, QStringLiteral("QMUD_HOME=/tmp/qmud-fallback-home\n"));

			ScopedEnvVar emptyHome(QByteArrayLiteral("QMUD_HOME"), QByteArray());
			qmudSetEnvironmentConfigFallbackEnabled(true);
			QCOMPARE(qmudEnvironmentVariable(QStringLiteral("QMUD_HOME")),
			         QStringLiteral("/tmp/qmud-fallback-home"));
		}

		void fallbackDisabledIgnoresConfigEvenWhenQmudHomeEnvIsEmpty()
		{
			QTemporaryDir tempDir;
			QVERIFY(tempDir.isValid());
			const QString                    configPath = tempDir.filePath(QStringLiteral("config"));
			const ScopedConfigSearchOverride configOverride(
			    configPath, QStringLiteral("QMUD_HOME=/tmp/qmud-fallback-home\n"));

			ScopedEnvVar emptyHome(QByteArrayLiteral("QMUD_HOME"), QByteArray());
			qmudSetEnvironmentConfigFallbackEnabled(false);
			QVERIFY(qmudEnvironmentVariable(QStringLiteral("QMUD_HOME")).isEmpty());
			qmudSetEnvironmentConfigFallbackEnabled(true);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_Environment)

#include "tst_Environment.moc"
