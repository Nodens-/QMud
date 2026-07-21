/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldRuntime_SoundLifetime.cpp
 * Role: Unit coverage for WorldRuntime audio backend lifetime handling.
 */

#include "WorldRuntime.h"

// ReSharper disable once CppUnusedIncludeDirective
#include <QFileInfo>
#include <QMediaPlayer>
#include <QSoundEffect>
#include <QTemporaryFile>
#include <QtTest/QTest>

#include <memory>

/**
 * @brief QTest fixture covering sound buffer backend teardown and stale-signal guards.
 */
class tst_WorldRuntime_SoundLifetime : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		/**
		 * @brief Verifies QSoundEffect callbacks only match the currently registered backend.
		 */
		void soundEffectSignalOwnerMustMatchCurrentSlot()
		{
			WorldRuntime runtime;

			auto        *effect      = reinterpret_cast<QSoundEffect *>(static_cast<quintptr>(0x1000));
			auto        *replacement = reinterpret_cast<QSoundEffect *>(static_cast<quintptr>(0x2000));
			WorldRuntime::SoundBuffer &entry = runtime.m_soundBuffers[0];
			entry.effect                     = effect;

			QCOMPARE(runtime.soundBufferForEffectSignal(effect, 1), &entry);

			entry.effect = nullptr;
			QVERIFY(!runtime.soundBufferForEffectSignal(effect, 1));

			entry.effect = replacement;
			QCOMPARE(runtime.soundBufferForEffectSignal(replacement, 1), &entry);
			QVERIFY(!runtime.soundBufferForEffectSignal(effect, 1));

			entry.effect = nullptr;
		}

		/**
		 * @brief Verifies QMediaPlayer callbacks only match the currently registered backend.
		 */
		void mediaPlayerSignalOwnerMustMatchCurrentSlot()
		{
			WorldRuntime runtime;

			auto        *player      = reinterpret_cast<QMediaPlayer *>(static_cast<quintptr>(0x3000));
			auto        *replacement = reinterpret_cast<QMediaPlayer *>(static_cast<quintptr>(0x4000));
			WorldRuntime::SoundBuffer &entry = runtime.m_soundBuffers[0];
			entry.player                     = player;

			QCOMPARE(runtime.soundBufferForPlayerSignal(player, 1), &entry);

			entry.player = nullptr;
			QVERIFY(!runtime.soundBufferForPlayerSignal(player, 1));

			entry.player = replacement;
			QCOMPARE(runtime.soundBufferForPlayerSignal(replacement, 1), &entry);
			QVERIFY(!runtime.soundBufferForPlayerSignal(player, 1));

			entry.player = nullptr;
		}

		/**
		 * @brief Verifies temporary memory-playback files are removed when their slot is cleared.
		 */
		void clearSoundBufferDeletesTemporaryMemoryFile()
		{
			WorldRuntime runtime;

			auto         temp = std::make_unique<QTemporaryFile>();
			QVERIFY(temp->open());
			QVERIFY(temp->write(QByteArrayLiteral("audio")) == 5);
			temp->flush();
			const QString tempFileName = temp->fileName();
			QVERIFY(QFileInfo::exists(tempFileName));

			WorldRuntime::SoundBuffer &entry = runtime.m_soundBuffers[0];
			entry.tempFile                   = temp.release();
			entry.playbackStarted            = true;
			entry.looping                    = true;

			WorldRuntime::clearSoundBuffer(entry);

			QVERIFY(!entry.effect);
			QVERIFY(!entry.player);
			QVERIFY(!entry.audioOutput);
			QVERIFY(!entry.tempFile);
			QVERIFY(!entry.playbackStarted);
			QVERIFY(!entry.looping);
			QCOMPARE(entry.volume, 1.0);
			QCOMPARE(entry.pan, 0.0);
			QVERIFY(!QFileInfo::exists(tempFileName));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_WorldRuntime_SoundLifetime)

#if __has_include("tst_WorldRuntime_SoundLifetime.moc")
#include "tst_WorldRuntime_SoundLifetime.moc"
#endif
