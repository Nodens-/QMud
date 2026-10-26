/**
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * @file SoundPcmStream.h
 * @brief Bounded streaming PCM transport and sample-clock playback controls.
 */

#ifndef QMUD_SOUNDPCMSTREAM_H
#define QMUD_SOUNDPCMSTREAM_H

#include "SoundEngine.h"

#if QMUD_ENABLE_SOUND
#include <QAudioBuffer>
#include <QSpan>
#include <array>
#include <chrono>
#include <mutex>
#include <vector>

/**
 * @brief Exchanges bounded PCM between the sound worker and Qt's audio callback.
 *
 * No callback touches a QObject, registry, session, or Lua state. The short transport lock has no outward calls.
 * Each playback assignment receives a fresh stream; cached decoders never retain PCM between assignments.
 */
class SoundPcmStream final
{
	public:
		/** @brief Coherent sample-clock control and queue state sampled by the sound worker. */
		struct State
		{
				double volume{1.0};
				double pan{0.0};
				double pitch{0.0};
				double playbackRate{1.0};
				qint64 lastOutputFrame{0};
				qint64 renderedFrames{0};
				bool   highWater{false};
				bool   lowWater{true};
				bool   drained{false};
				bool   faded{false};
		};

		/**
		 * @brief Creates one playback transport, retaining neutral defaults for non-finite initial controls.
		 * @param outputFormat Negotiated sink format copied into this transport.
		 * @param volume Initial linear gain.
		 * @param pan Initial balance from -100 through 100.
		 * @param pitch Initial frequency offset in units of 100 Hz.
		 */
		SoundPcmStream(const QAudioFormat &outputFormat, double volume, double pan, double pitch);
		/**
		 * @brief Appends one decoder buffer without allowing the transport to grow past its bound.
		 * @param buffer Native-format PCM emitted by the decoder.
		 * @return `true` when the buffer was accepted, or `false` for invalid format or overflow.
		 */
		[[nodiscard]] bool append(const QAudioBuffer &buffer);
		/** @brief Marks the final decoder buffer as submitted. */
		void               finish();
		/** @brief Reopens decoder input without changing queued PCM, resampling phase, or sample-clock controls. */
		void               reopenInput();
		/**
		 * @brief Replaces one attribute's current envelope without disturbing other attributes or accepting non-finite targets.
		 * @param attribute Attribute to change.
		 * @param value Target in LuaAudio units, with volume expressed as linear gain.
		 * @param duration Nonnegative slide duration.
		 * @param stopAtEnd Whether completing this volume envelope ends playback.
		 * @param targetMode Whether the target offsets the current value under the transport lock.
		 */
		void setAttribute(QMudSoundAttribute attribute, double value, std::chrono::milliseconds duration,
		                  bool                     stopAtEnd  = false,
		                  QMudSoundAttributeTarget targetMode = QMudSoundAttributeTarget::Absolute);
		/**
		 * @brief Changes an active non-muted voice only when its current gain differs, preserving no-op fades.
		 * @param volume Global linear gain to apply.
		 */
		void setGlobalVolume(double volume);
		/**
		 * @brief Returns coherent current controls and transport state.
		 * @return State at the last rendered sample frame.
		 */
		[[nodiscard]] State state() const;
		/**
		 * @brief Renders floating-point output on Qt's audio callback thread or a deterministic test clock.
		 * @param output Interleaved sink samples to fill completely.
		 */
		void                render(QSpan<float> output);
		/**
		 * @brief Renders signed 16-bit output using the same sample-clock processing.
		 * @param output Interleaved sink samples to fill completely.
		 */
		void                render(QSpan<qint16> output);
		/**
		 * @brief Renders signed 32-bit output using the same sample-clock processing.
		 * @param output Interleaved sink samples to fill completely.
		 */
		void                render(QSpan<qint32> output);
		/**
		 * @brief Renders unsigned 8-bit output using the same sample-clock processing.
		 * @param output Interleaved sink samples to fill completely.
		 */
		void                render(QSpan<quint8> output);

	private:
		/**
		 * @brief Replaces one envelope during construction or while the caller holds the transport lock.
		 * @param attribute Attribute to change.
		 * @param value New absolute value or relative offset.
		 * @param duration Slide duration.
		 * @param stopAtEnd Whether the volume envelope ends playback.
		 * @param targetMode Whether to add the offset to the sampled current value.
		 */
		void setAttributeLocked(QMudSoundAttribute attribute, double value,
		                        std::chrono::milliseconds duration, bool stopAtEnd,
		                        QMudSoundAttributeTarget targetMode);
		/** @brief Linear attribute envelope expressed in output frames. */
		struct Envelope
		{
				double               start{0.0};
				double               target{0.0};
				qint64               firstFrame{0};
				qint64               lastFrame{0};
				/**
				 * @brief Samples an envelope without mutating its endpoints.
				 * @param frame Output frame to sample.
				 * @return Interpolated value.
				 */
				[[nodiscard]] double value(qint64 frame) const;
		};
		/**
		 * @brief Renders interleaved output with the negotiated integer or floating sample representation.
		 * @tparam Sample Sink sample representation.
		 * @param output Interleaved samples to fill.
		 */
		template <typename Sample> void   renderSamples(QSpan<Sample> output);

		QAudioFormat                      m_outputFormat;
		mutable std::mutex                m_mutex;
		std::vector<std::array<float, 2>> m_frames;
		std::array<Envelope, 3>           m_attributes;
		qsizetype                         m_head{0};
		qsizetype                         m_size{0};
		int                               m_sourceRate{0};
		double                            m_sourcePosition{0.0};
		qint64                            m_outputFrame{0};
		qint64                            m_lastOutputFrame{0};
		bool                              m_finished{false};
		bool                              m_stopAfterFade{false};
};
#endif
#endif // QMUD_SOUNDPCMSTREAM_H
