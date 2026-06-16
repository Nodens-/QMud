/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SoundFileRoutingUtils.cpp
 * Role: Sound backend routing helpers for file-based playback.
 */

#include "SoundFileRoutingUtils.h"

#include <QByteArray>
// ReSharper disable once CppUnusedIncludeDirective
#include <QFile>
#include <QFileInfo>
#include <QIODevice>

#include <algorithm>
#include <array>

namespace
{
	/**
	 * @brief Reads an unsigned 16-bit little-endian integer from byte data.
	 * @param data Source byte data.
	 * @return Decoded integer.
	 */
	quint16 littleEndianUInt16(const QByteArray &data)
	{
		return static_cast<quint16>(static_cast<quint8>(data.at(0))) |
		       static_cast<quint16>(static_cast<quint8>(data.at(1)) << 8U);
	}

	/**
	 * @brief Reads an unsigned 32-bit little-endian integer from WAVE chunk-header size bytes.
	 * @param data Source byte data.
	 * @return Decoded integer.
	 */
	quint32 littleEndianChunkSize(const QByteArray &data)
	{
		return static_cast<quint32>(static_cast<quint8>(data.at(4))) |
		       (static_cast<quint32>(static_cast<quint8>(data.at(5))) << 8U) |
		       (static_cast<quint32>(static_cast<quint8>(data.at(6))) << 16U) |
		       (static_cast<quint32>(static_cast<quint8>(data.at(7))) << 24U);
	}

	/**
	 * @brief Checks if a WAVE fmt chunk represents a simple QSoundEffect-friendly format.
	 * @param formatChunk WAVE fmt chunk payload.
	 * @return `true` when the format should remain on QSoundEffect.
	 */
	bool waveFormatChunkUsesSoundEffect(const QByteArray &formatChunk)
	{
		constexpr quint16              kWaveFormatPcm                 = 0x0001;
		constexpr quint16              kWaveFormatExtensible          = 0xFFFE;
		constexpr qsizetype            kWaveExtensibleSubFormatOffset = 24;
		constexpr std::array<char, 16> kPcmSubFormatGuid{'\x01', '\x00', '\x00',
		                                                 '\x00', '\x00', '\x00',
		                                                 '\x10', '\x00', static_cast<char>(0x80),
		                                                 '\x00', '\x00', static_cast<char>(0xAA),
		                                                 '\x00', '\x38', static_cast<char>(0x9B),
		                                                 '\x71'};

		if (formatChunk.size() < 2)
			return false;

		const quint16 formatTag = littleEndianUInt16(formatChunk);
		if (formatTag == kWaveFormatPcm)
			return true;
		if (formatTag != kWaveFormatExtensible)
			return false;
		if (formatChunk.size() <
		    kWaveExtensibleSubFormatOffset + static_cast<qsizetype>(kPcmSubFormatGuid.size()))
			return false;

		return std::equal(kPcmSubFormatGuid.cbegin(), kPcmSubFormatGuid.cend(),
		                  formatChunk.constData() + kWaveExtensibleSubFormatOffset);
	}

	/**
	 * @brief Checks if a WAVE file should use QSoundEffect.
	 * @param fileName Absolute file path to inspect.
	 * @return `true` when QSoundEffect is appropriate.
	 */
	bool waveFileUsesSoundEffect(const QString &fileName)
	{
		QFile file(fileName);
		if (!file.open(QIODevice::ReadOnly))
			return false;

		const QByteArray riffHeader = file.read(12);
		if (riffHeader.size() != 12 || riffHeader.first(4) != QByteArrayLiteral("RIFF") ||
		    riffHeader.sliced(8, 4) != QByteArrayLiteral("WAVE"))
			return false;

		const qint64 fileSize = file.size();
		while (file.pos() + 8 <= fileSize)
		{
			const QByteArray chunkHeader = file.read(8);
			if (chunkHeader.size() != 8)
				return false;

			const QByteArray chunkId   = chunkHeader.first(4);
			const quint32    chunkSize = littleEndianChunkSize(chunkHeader);
			if (chunkId == QByteArrayLiteral("fmt "))
			{
				const qint64     bytesToRead = qMin<qint64>(chunkSize, 40);
				const QByteArray formatChunk = file.read(bytesToRead);
				return waveFormatChunkUsesSoundEffect(formatChunk);
			}

			const qint64 nextChunkPosition = file.pos() + static_cast<qint64>(chunkSize) + (chunkSize % 2U);
			if (nextChunkPosition < file.pos() || nextChunkPosition > fileSize ||
			    !file.seek(nextChunkPosition))
				return false;
		}

		return false;
	}
} // namespace

SoundFilePlaybackBackend soundFilePlaybackBackendForFile(const QString &fileName)
{
	const QString suffix = QFileInfo(fileName).suffix().toLower();
	if (suffix != QStringLiteral("wav") && suffix != QStringLiteral("wave"))
		return SoundFilePlaybackBackend::QMediaPlayer;

	return waveFileUsesSoundEffect(fileName) ? SoundFilePlaybackBackend::QSoundEffect
	                                         : SoundFilePlaybackBackend::QMediaPlayer;
}
