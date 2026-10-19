/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_SoundFileRoutingUtils.cpp
 * Role: Unit coverage for sound path validation and playback-capability helpers.
 */

#include "SoundFileRoutingUtils.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>

#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Writes a temporary sound file for routing tests.
	 * @param dir Temporary directory.
	 * @param fileName File name below the temporary directory.
	 * @param bytes File bytes.
	 * @return Absolute file path.
	 */
	QString writeSoundFile(const QTemporaryDir &dir, const QString &fileName, const QByteArray &bytes)
	{
		const QString path = QDir(dir.path()).filePath(fileName);
		QFile         file(path);
		if (!file.open(QIODevice::WriteOnly))
			return {};
		if (file.write(bytes) != static_cast<qint64>(bytes.size()))
			return {};
		return file.fileName();
	}
	/**
	 * @brief QTest fixture for sound file routing.
	 */
	class tst_SoundFileRoutingUtils : public QObject
	{
			Q_OBJECT

		private slots:
			/** @brief Verifies world sound directories normalize beneath the shared sounds tree. */
			static void worldSoundDirectoryIsNormalizedAndContained()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-directory-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));

				WorldSoundDirectoryPaths paths;
				QVERIFY(resolveWorldSoundDirectory(root.path(), {}, &paths));
				QCOMPARE(paths.storedPath, QStringLiteral("./sounds/"));
				QCOMPARE(paths.absolutePath, QDir(root.path()).filePath(QStringLiteral("sounds")));
				QVERIFY(paths.catalogPrefix.isEmpty());

				QVERIFY(resolveWorldSoundDirectory(root.path(), QStringLiteral("sounds/world/zone"), &paths));
				QCOMPARE(paths.storedPath, QStringLiteral("./sounds/world/zone/"));
				QCOMPARE(paths.catalogPrefix, QStringLiteral("world/zone"));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds/Actual-World/Zone")));
				QVERIFY(resolveWorldSoundDirectory(
				    root.path(), QStringLiteral("./SOUNDS/actual-world/zone/missing"), &paths));
				QCOMPARE(paths.storedPath, QStringLiteral("./sounds/Actual-World/Zone/missing/"));
				QCOMPARE(paths.absolutePath,
				         QDir(root.path()).filePath(QStringLiteral("sounds/Actual-World/Zone/missing")));
				QCOMPARE(paths.catalogPrefix, QStringLiteral("Actual-World/Zone/missing"));
				QVERIFY(resolveWorldSoundDirectory(root.path(), QStringLiteral(R"(.\sounds\world\zone)"),
				                                   &paths));
				QCOMPARE(paths.storedPath, QStringLiteral("./sounds/world/zone/"));
				QVERIFY(resolveWorldSoundDirectory(
				    root.path(), QStringLiteral(R"(C:\MUSHclient\sounds\world\zone)"), &paths));
				QCOMPARE(paths.catalogPrefix, QStringLiteral("world/zone"));
				QVERIFY(!resolveWorldSoundDirectory(root.path(), QStringLiteral("./logs"), &paths));
				QVERIFY(
				    !resolveWorldSoundDirectory(root.path(), QStringLiteral("./sounds/../../logs"), &paths));
				QVERIFY(resolveWorldSoundDirectory(
				    root.path(), QDir(root.path()).filePath(QStringLiteral("sounds/world")), &paths));
				QCOMPARE(paths.storedPath, QStringLiteral("./sounds/world/"));
				QFile soundFile(QDir(root.path()).filePath(QStringLiteral("sounds/not-a-directory.wav")));
				QVERIFY(soundFile.open(QIODevice::WriteOnly));
				soundFile.close();
				QVERIFY(!resolveWorldSoundDirectory(root.path(),
				                                    QStringLiteral("./sounds/not-a-directory.wav"), &paths));

				bool explicitlyRooted = false;
				QCOMPARE(soundTreeRelativePath(QStringLiteral(R"(combat\hit.wav)"), &explicitlyRooted),
				         QStringLiteral("combat/hit.wav"));
				QVERIFY(!explicitlyRooted);
				QCOMPARE(soundTreeRelativePath(QStringLiteral(R"(C:\MUSHclient\sounds\combat\hit.wav)"),
				                               &explicitlyRooted),
				         QStringLiteral("combat/hit.wav"));
				QVERIFY(explicitlyRooted);
			}

			/** @brief Verifies an existing symlink cannot redirect a world sound directory outside sounds. */
			static void worldSoundDirectoryRejectsSymlinkEscape()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-directory-link-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds")));
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("outside")));
				const QString link = QDir(root.path()).filePath(QStringLiteral("sounds/escape"));
				QVERIFY(QFile::link(QDir(root.path()).filePath(QStringLiteral("outside")), link));

				WorldSoundDirectoryPaths paths;
				QVERIFY(!resolveWorldSoundDirectory(root.path(), QStringLiteral("./sounds/escape"), &paths));

				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sounds/real")));
				const QString containedLink =
				    QDir(root.path()).filePath(QStringLiteral("sounds/contained-link"));
				if (!QFile::link(QDir(root.path()).filePath(QStringLiteral("sounds/real")), containedLink))
					QSKIP("Directory symlinks are unavailable on this platform");
				QVERIFY(!resolveWorldSoundDirectory(root.path(), QStringLiteral("./sounds/contained-link"),
				                                    &paths));
				QVERIFY(soundPathContainsSymlink(QDir(root.path()).filePath(QStringLiteral("sounds")),
				                                 QDir(containedLink).filePath(QStringLiteral("file.wav"))));
				QVERIFY(soundPathContainsSymlink(
				    containedLink, QDir(root.path()).filePath(QStringLiteral("sounds/real/file.wav"))));
				QVERIFY(!soundPathContainsSymlink(QDir(root.path()).filePath(QStringLiteral("sounds")),
				                                  QDir(root.path()).filePath(QStringLiteral("sounds/real"))));
			}

			/** @brief Verifies exact audio-file resolution is case-insensitive, contained, and symlink-safe. */
			static void existingSoundFileResolutionPreservesContainment()
			{
				QTemporaryDir root(QDir::current().filePath(QStringLiteral("sound-file-routing-XXXXXX")));
				QVERIFY(root.isValid());
				QVERIFY(QDir(root.path()).mkpath(QStringLiteral("Plugin/Audio")));
				const QString sound = writeSoundFile(root, QStringLiteral("Plugin/Audio/Prompt.WaV"),
				                                     QByteArrayLiteral("sound"));
				QVERIFY(!sound.isEmpty());
				const QString text = writeSoundFile(root, QStringLiteral("Plugin/Audio/readme.txt"),
				                                    QByteArrayLiteral("not audio"));
				QVERIFY(!text.isEmpty());

				QString resolved;
				QVERIFY(resolveSoundFileCaseInsensitively(
				    root.path(), QStringLiteral(R"(plugin\audio\prompt.wav)"), &resolved));
				QCOMPARE(resolved, sound);
				QVERIFY(!resolveSoundFileCaseInsensitively(
				    root.path(), QStringLiteral("Plugin/Audio/readme.txt"), &resolved));
				QVERIFY(!resolveSoundFileCaseInsensitively(
				    root.path(), QStringLiteral("../outside/Prompt.WaV"), &resolved));

				const QString link = QDir(root.path()).filePath(QStringLiteral("Plugin/Audio/linked.wav"));
				if (QFile::link(sound, link))
					QVERIFY(!resolveSoundFileCaseInsensitively(
					    root.path(), QStringLiteral("Plugin/Audio/linked.wav"), &resolved));
			}

			/**
			 * @brief Verifies playback suffix validation follows Qt Multimedia decode capabilities.
			 */
			static void supportedSuffixesFollowMultimediaCapabilities()
			{
				const QSet<QString> &suffixes = soundPlaybackSupportedFileSuffixes();
#if QMUD_ENABLE_SOUND
				QVERIFY(!suffixes.isEmpty());
				const QString suffix = *suffixes.cbegin();
				QVERIFY(soundPlaybackSupportsFile(QStringLiteral("effect.%1").arg(suffix)));
				QVERIFY(soundPlaybackSupportsFile(QStringLiteral("effect.%1").arg(suffix.toUpper())));
				QVERIFY(!soundPlaybackSupportsFile(QStringLiteral("effect.qmud_unsupported_audio")));
				QVERIFY(!soundPlaybackSupportsFile(QStringLiteral("effect")));
#else
				QVERIFY(suffixes.isEmpty());
				QVERIFY(!soundPlaybackSupportsFile(QStringLiteral("effect.wav")));
#endif
			}
	};

} // namespace

QTEST_GUILESS_MAIN(tst_SoundFileRoutingUtils)

#include "tst_SoundFileRoutingUtils.moc"
