/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SoundFileRoutingUtils.h
 * Role: Sound backend routing helpers for file-based playback.
 */

#ifndef QMUD_SOUNDFILEROUTINGUTILS_H
#define QMUD_SOUNDFILEROUTINGUTILS_H

#include <QString>

/**
 * @brief Playback backend selected for a sound file.
 */
enum class SoundFilePlaybackBackend
{
	QSoundEffect,
	QMediaPlayer
};

/**
 * @brief Chooses the Qt playback backend for a sound file.
 * @param fileName Absolute file path to inspect.
 * @return Backend appropriate for the file format.
 */
[[nodiscard]] SoundFilePlaybackBackend soundFilePlaybackBackendForFile(const QString &fileName);

#endif // QMUD_SOUNDFILEROUTINGUTILS_H
