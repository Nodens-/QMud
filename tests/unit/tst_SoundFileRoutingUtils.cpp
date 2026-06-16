/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_SoundFileRoutingUtils.cpp
 * Role: Unit coverage for sound file backend routing helpers.
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
	 * @brief Appends a 16-bit little-endian value to byte data.
	 * @param data Mutable destination data.
	 * @param value Value to append.
	 */
	void appendLe16(QByteArray &data, const quint16 value)
	{
		data.append(static_cast<char>(value & 0xFFU));
		data.append(static_cast<char>((value >> 8U) & 0xFFU));
	}

	/**
	 * @brief Appends a 32-bit little-endian value to byte data.
	 * @param data Mutable destination data.
	 * @param value Value to append.
	 */
	void appendLe32(QByteArray &data, const quint32 value)
	{
		data.append(static_cast<char>(value & 0xFFU));
		data.append(static_cast<char>((value >> 8U) & 0xFFU));
		data.append(static_cast<char>((value >> 16U) & 0xFFU));
		data.append(static_cast<char>((value >> 24U) & 0xFFU));
	}

	/**
	 * @brief Builds a minimal RIFF/WAVE file payload.
	 * @param formatTag WAVE format tag.
	 * @param extensiblePcm Whether to include an extensible PCM subformat GUID.
	 * @return Encoded WAVE file bytes.
	 */
	QByteArray waveBytes(const quint16 formatTag, const bool extensiblePcm = false)
	{
		QByteArray formatChunk;
		appendLe16(formatChunk, formatTag);
		appendLe16(formatChunk, 1);
		appendLe32(formatChunk, 44100);
		appendLe32(formatChunk, 88200);
		appendLe16(formatChunk, 2);
		appendLe16(formatChunk, 16);
		if (extensiblePcm)
		{
			appendLe16(formatChunk, 22);
			appendLe16(formatChunk, 16);
			appendLe32(formatChunk, 3);
			formatChunk.append(QByteArray::fromHex("0100000000001000800000AA00389B71"));
		}

		QByteArray data;
		data.append("RIFF", 4);
		appendLe32(data, static_cast<quint32>(4 + 8 + formatChunk.size() + 8));
		data.append("WAVE", 4);
		data.append("fmt ", 4);
		appendLe32(data, static_cast<quint32>(formatChunk.size()));
		data.append(formatChunk);
		if ((formatChunk.size() % 2) != 0)
			data.append('\0');
		data.append("data", 4);
		appendLe32(data, 0);
		return data;
	}

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
} // namespace

/**
 * @brief QTest fixture for sound file playback backend routing.
 */
class tst_SoundFileRoutingUtils : public QObject
{
		Q_OBJECT

	private slots:
		/**
		 * @brief Verifies ordinary PCM WAV files are routed to QSoundEffect.
		 */
		static void pcmWaveUsesSoundEffect()
		{
			QTemporaryDir dir;
			QVERIFY(dir.isValid());

			const QString path = writeSoundFile(dir, QStringLiteral("plain.wav"), waveBytes(0x0001));
			QVERIFY(!path.isEmpty());

			QCOMPARE(soundFilePlaybackBackendForFile(path), SoundFilePlaybackBackend::QSoundEffect);
		}

		/**
		 * @brief Verifies extensible PCM WAV files are routed to QSoundEffect.
		 */
		static void extensiblePcmWaveUsesSoundEffect()
		{
			QTemporaryDir dir;
			QVERIFY(dir.isValid());

			const QString path =
			    writeSoundFile(dir, QStringLiteral("extensible.wave"), waveBytes(0xFFFE, true));
			QVERIFY(!path.isEmpty());

			QCOMPARE(soundFilePlaybackBackendForFile(path), SoundFilePlaybackBackend::QSoundEffect);
		}

		/**
		 * @brief Verifies compressed WAV containers are routed to QMediaPlayer.
		 */
		static void compressedWaveUsesMediaPlayer()
		{
			QTemporaryDir dir;
			QVERIFY(dir.isValid());

			const QString path = writeSoundFile(dir, QStringLiteral("compressed.wav"), waveBytes(0x0055));
			QVERIFY(!path.isEmpty());

			QCOMPARE(soundFilePlaybackBackendForFile(path), SoundFilePlaybackBackend::QMediaPlayer);
		}

		/**
		 * @brief Verifies malformed WAV files are routed away from QSoundEffect.
		 */
		static void malformedWaveUsesMediaPlayer()
		{
			QTemporaryDir dir;
			QVERIFY(dir.isValid());

			const QString path =
			    writeSoundFile(dir, QStringLiteral("broken.wav"), QByteArrayLiteral("not wave"));
			QVERIFY(!path.isEmpty());

			QCOMPARE(soundFilePlaybackBackendForFile(path), SoundFilePlaybackBackend::QMediaPlayer);
		}

		/**
		 * @brief Verifies non-WAV file names use QMediaPlayer.
		 */
		static void nonWaveUsesMediaPlayer()
		{
			QTemporaryDir dir;
			QVERIFY(dir.isValid());

			const QString path = writeSoundFile(dir, QStringLiteral("music.ogg"), QByteArrayLiteral("OggS"));
			QVERIFY(!path.isEmpty());

			QCOMPARE(soundFilePlaybackBackendForFile(path), SoundFilePlaybackBackend::QMediaPlayer);
		}
};

QTEST_APPLESS_MAIN(tst_SoundFileRoutingUtils)

#if __has_include("tst_SoundFileRoutingUtils.moc")
#include "tst_SoundFileRoutingUtils.moc"
#endif
