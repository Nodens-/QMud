/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TestSoundData.h
 * Role: Shared valid audio fixtures for tests that exercise real Qt sound backends.
 */

#ifndef QMUD_TESTSOUNDDATA_H
#define QMUD_TESTSOUNDDATA_H

#include <QByteArray>
#include <QtTypes>

/**
 * @brief Builds valid in-memory audio fixtures for Qt sound tests.
 */
namespace QMudTestSoundData
{
	/**
	 * @brief Appends one unsigned 16-bit value in RIFF little-endian order.
	 * @param bytes Destination byte array.
	 * @param value Value to append.
	 */
	inline void appendLittleEndian16(QByteArray &bytes, const quint16 value)
	{
		bytes.append(static_cast<char>(value & 0xFFU));
		bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
	}

	/**
	 * @brief Appends one unsigned 32-bit value in RIFF little-endian order.
	 * @param bytes Destination byte array.
	 * @param value Value to append.
	 */
	inline void appendLittleEndian32(QByteArray &bytes, const quint32 value)
	{
		bytes.append(static_cast<char>(value & 0xFFU));
		bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
		bytes.append(static_cast<char>((value >> 16U) & 0xFFU));
		bytes.append(static_cast<char>((value >> 24U) & 0xFFU));
	}

	/**
	 * @brief Creates a valid mono PCM wave containing silence.
	 * @param durationMilliseconds Requested duration, clamped to at least one millisecond.
	 * @return Complete in-memory RIFF/WAVE payload accepted by Qt audio backends.
	 */
	[[nodiscard]] inline QByteArray silentPcmWave(const int durationMilliseconds = 2000)
	{
		constexpr quint32 kSampleRate    = 8000;
		constexpr quint16 kChannelCount  = 1;
		constexpr quint16 kBitsPerSample = 16;
		constexpr quint16 kBlockAlign    = kChannelCount * (kBitsPerSample / 8);
		constexpr quint32 kByteRate      = kSampleRate * kBlockAlign;
		const quint32     dataSize = kByteRate * static_cast<quint32>(qMax(1, durationMilliseconds)) / 1000U;

		QByteArray        bytes;
		bytes.reserve(static_cast<qsizetype>(44U + dataSize));
		bytes.append("RIFF", 4);
		appendLittleEndian32(bytes, 36U + dataSize);
		bytes.append("WAVE", 4);
		bytes.append("fmt ", 4);
		appendLittleEndian32(bytes, 16U);
		appendLittleEndian16(bytes, 1U);
		appendLittleEndian16(bytes, kChannelCount);
		appendLittleEndian32(bytes, kSampleRate);
		appendLittleEndian32(bytes, kByteRate);
		appendLittleEndian16(bytes, kBlockAlign);
		appendLittleEndian16(bytes, kBitsPerSample);
		bytes.append("data", 4);
		appendLittleEndian32(bytes, dataSize);
		bytes.append(static_cast<qsizetype>(dataSize), '\0');
		return bytes;
	}
} // namespace QMudTestSoundData

#endif // QMUD_TESTSOUNDDATA_H
