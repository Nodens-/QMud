/**
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * @file SoundPcmStream.cpp
 * @brief Streaming balance, gain, frequency conversion, and sample-clock attribute envelopes.
 */

#include "SoundPcmStream.h"

#if QMUD_ENABLE_SOUND
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace
{
	/**
	 * @brief Converts one normalized sample to the negotiated sink representation.
	 * @tparam Sample Output sample type.
	 * @param value Normalized sample value.
	 * @return Clamped sink sample.
	 */
	template <typename Sample> Sample sinkSample(const float value)
	{
		const float bounded = std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
		if constexpr (std::is_same_v<Sample, float>)
			return bounded;
		else if constexpr (std::is_same_v<Sample, quint8>)
			return static_cast<quint8>(std::lround((static_cast<double>(bounded) + 1.0) * 127.5));
		else
			return static_cast<Sample>(
			    std::llround(static_cast<double>(bounded) * std::numeric_limits<Sample>::max()));
	}

	/**
	 * @brief Produces standard stereo downmix coefficients from the source's channel layout.
	 * @param format Decoder sample format and channel positions.
	 * @return Per-input-channel left and right gains.
	 */
	std::vector<std::array<float, 2>> stereoGains(QAudioFormat format)
	{
		const int                         channels = format.channelCount();
		std::vector<std::array<float, 2>> gains(static_cast<std::size_t>(channels));
		if (channels == 1)
		{
			gains.front() = {1.0F, 1.0F};
			return gains;
		}
		if (format.channelConfig() == QAudioFormat::ChannelConfigUnknown)
			format.setChannelConfig(QAudioFormat::defaultChannelConfigForChannelCount(channels));
		for (int position = QAudioFormat::FrontLeft; position < QAudioFormat::NChannelPositions; ++position)
		{
			const auto channel = static_cast<QAudioFormat::AudioChannelPosition>(position);
			const int  index   = format.channelOffset(channel);
			if (index < 0 || index >= channels)
				continue;
			std::array<float, 2> gain{0.70710678F, 0.70710678F};
			switch (channel)
			{
			case QAudioFormat::FrontLeft:
				gain = {1.0F, 0.0F};
				break;
			case QAudioFormat::FrontRight:
				gain = {0.0F, 1.0F};
				break;
			case QAudioFormat::BackLeft:
			case QAudioFormat::SideLeft:
			case QAudioFormat::FrontLeftOfCenter:
			case QAudioFormat::TopFrontLeft:
			case QAudioFormat::TopBackLeft:
			case QAudioFormat::TopSideLeft:
			case QAudioFormat::BottomFrontLeft:
				gain = {0.70710678F, 0.0F};
				break;
			case QAudioFormat::BackRight:
			case QAudioFormat::SideRight:
			case QAudioFormat::FrontRightOfCenter:
			case QAudioFormat::TopFrontRight:
			case QAudioFormat::TopBackRight:
			case QAudioFormat::TopSideRight:
			case QAudioFormat::BottomFrontRight:
				gain = {0.0F, 0.70710678F};
				break;
			case QAudioFormat::LFE:
			case QAudioFormat::LFE2:
				gain = {0.5F, 0.5F};
				break;
			default:
				break;
			}
			gains[static_cast<std::size_t>(index)] = gain;
		}
		return gains;
	}
} // namespace

SoundPcmStream::SoundPcmStream(const QAudioFormat &outputFormat, const double volume, const double pan,
                               const double pitch)
    : m_outputFormat(outputFormat)
{
	m_attributes[0].start = m_attributes[0].target = 1.0;
	setAttributeLocked(QMudSoundAttribute::Volume, volume, {}, false, QMudSoundAttributeTarget::Absolute);
	setAttributeLocked(QMudSoundAttribute::Pan, pan, {}, false, QMudSoundAttributeTarget::Absolute);
	setAttributeLocked(QMudSoundAttribute::Pitch, pitch, {}, false, QMudSoundAttributeTarget::Absolute);
}

double SoundPcmStream::Envelope::value(const qint64 frame) const
{
	if (frame >= lastFrame || lastFrame <= firstFrame)
		return target;
	if (frame <= firstFrame)
		return start;
	return std::lerp(start, target,
	                 static_cast<double>(frame - firstFrame) / static_cast<double>(lastFrame - firstFrame));
}

bool SoundPcmStream::append(const QAudioBuffer &buffer)
{
	const QAudioFormat format = buffer.format();
	if (!buffer.isValid() || !format.isValid() || format.channelCount() > QAudioFormat::NChannelPositions ||
	    format.sampleRate() > 384'000 || buffer.frameCount() > format.sampleRate() * 4)
		return false;
	const auto                        gains = stereoGains(format);
	std::vector<std::array<float, 2>> converted(static_cast<std::size_t>(buffer.frameCount()));
	const char                       *samples = buffer.constData<char>();
	for (auto &frame : converted)
	{
		for (int channel = 0; channel < format.channelCount(); ++channel)
		{
			const float sample = format.normalizedSampleValue(samples);
			samples += format.bytesPerSample();
			frame[0] += sample * gains[static_cast<std::size_t>(channel)][0];
			frame[1] += sample * gains[static_cast<std::size_t>(channel)][1];
		}
	}
	std::vector<std::array<float, 2>> initialStorage;
	if (m_sourceRate == 0)
		initialStorage.resize(static_cast<std::size_t>(format.sampleRate()) * 4);
	const std::lock_guard lock(m_mutex);
	if (m_sourceRate == 0)
	{
		m_sourceRate = format.sampleRate();
		m_frames     = std::move(initialStorage);
	}
	if (m_sourceRate != format.sampleRate() ||
	    converted.size() > m_frames.size() - static_cast<std::size_t>(m_size))
		return false;
	const auto tail = (static_cast<std::size_t>(m_head) + static_cast<std::size_t>(m_size)) % m_frames.size();
	const auto first = std::min(converted.size(), m_frames.size() - tail);
	std::copy_n(converted.begin(), first, m_frames.begin() + static_cast<std::ptrdiff_t>(tail));
	std::copy(converted.begin() + static_cast<std::ptrdiff_t>(first), converted.end(), m_frames.begin());
	m_size += static_cast<qsizetype>(converted.size());
	return true;
}

void SoundPcmStream::finish()
{
	const std::lock_guard lock(m_mutex);
	m_finished = true;
}

void SoundPcmStream::reopenInput()
{
	const std::lock_guard lock(m_mutex);
	m_finished = false;
}

void SoundPcmStream::setAttribute(const QMudSoundAttribute attribute, const double value,
                                  const std::chrono::milliseconds duration, const bool stopAtEnd,
                                  const QMudSoundAttributeTarget targetMode)
{
	const std::lock_guard lock(m_mutex);
	setAttributeLocked(attribute, value, duration, stopAtEnd, targetMode);
}

void SoundPcmStream::setGlobalVolume(const double volume)
{
	const std::lock_guard lock(m_mutex);
	const double          current = m_attributes[0].value(m_outputFrame);
	if (current != 0.0 && !qFuzzyCompare(current, volume))
		setAttributeLocked(QMudSoundAttribute::Volume, volume, {}, false, QMudSoundAttributeTarget::Absolute);
}

void SoundPcmStream::setAttributeLocked(const QMudSoundAttribute attribute, const double value,
                                        const std::chrono::milliseconds duration, const bool stopAtEnd,
                                        const QMudSoundAttributeTarget targetMode)
{
	auto        &envelope = m_attributes[static_cast<std::size_t>(attribute)];
	const double current  = envelope.value(m_outputFrame);
	const double target   = targetMode == QMudSoundAttributeTarget::Relative ? current + value : value;
	if (!std::isfinite(target))
		return;
	envelope.start      = current;
	envelope.target     = target;
	envelope.firstFrame = m_outputFrame;
	envelope.lastFrame  = m_outputFrame + duration.count() * m_outputFormat.sampleRate() / 1000;
	if (attribute == QMudSoundAttribute::Volume)
		m_stopAfterFade = stopAtEnd;
}

SoundPcmStream::State SoundPcmStream::state() const
{
	const std::lock_guard lock(m_mutex);
	State                 result;
	result.volume = m_attributes[0].value(m_outputFrame);
	result.pan    = m_attributes[1].value(m_outputFrame);
	result.pitch  = m_attributes[2].value(m_outputFrame);
	if (m_sourceRate > 0)
		result.playbackRate = std::clamp((m_sourceRate + result.pitch * 100.0) / m_sourceRate, 0.01, 16.0);
	result.highWater       = m_size >= m_sourceRate && m_sourceRate > 0;
	result.lowWater        = m_size < m_sourceRate / 4;
	result.drained         = m_finished && m_size == 0;
	result.faded           = m_stopAfterFade && m_outputFrame >= m_attributes[0].lastFrame;
	result.lastOutputFrame = m_lastOutputFrame;
	result.renderedFrames  = m_outputFrame;
	return result;
}

template <typename Sample> void SoundPcmStream::renderSamples(const QSpan<Sample> output)
{
	const std::lock_guard lock(m_mutex);
	const int             channels = m_outputFormat.channelCount();
	for (qsizetype index = 0; index < output.size(); index += channels)
	{
		if (m_size > 0 && m_sourcePosition >= 1.0)
		{
			const auto skipped = std::min(static_cast<qsizetype>(m_sourcePosition), m_size);
			m_sourcePosition -= static_cast<double>(skipped);
			m_head = (m_head + skipped) % static_cast<qsizetype>(m_frames.size());
			m_size -= skipped;
		}
		std::array<float, 2> frame{};
		const bool           faded = m_stopAfterFade && m_outputFrame >= m_attributes[0].lastFrame;
		if (m_size > 0 && !faded && (m_size > 1 || m_finished || m_sourcePosition == 0.0))
		{
			const auto &first = m_frames[static_cast<std::size_t>(m_head)];
			const auto &second =
			    m_frames[(static_cast<std::size_t>(m_head) + (m_size > 1 ? 1U : 0U)) % m_frames.size()];
			const double gain = m_attributes[0].value(m_outputFrame);
			const double pan  = std::clamp(m_attributes[1].value(m_outputFrame) / 100.0, -1.0, 1.0);
			frame[0]          = static_cast<float>(std::lerp(first[0], second[0], m_sourcePosition) * gain *
			                                       (pan > 0 ? 1.0 - pan : 1.0));
			frame[1]          = static_cast<float>(std::lerp(first[1], second[1], m_sourcePosition) * gain *
			                                       (pan < 0 ? 1.0 + pan : 1.0));
			const double rate = std::clamp(
			    (m_sourceRate + m_attributes[2].value(m_outputFrame) * 100.0) / m_sourceRate, 0.01, 16.0);
			m_sourcePosition += rate * m_sourceRate / m_outputFormat.sampleRate();
			const auto consumed = std::min(static_cast<qsizetype>(m_sourcePosition), m_size);
			m_sourcePosition -= static_cast<double>(consumed);
			m_head = (m_head + consumed) % static_cast<qsizetype>(m_frames.size());
			m_size -= consumed;
			m_lastOutputFrame = m_outputFrame + 1;
		}
		for (int channel = 0; channel < channels && index + channel < output.size(); ++channel)
		{
			const float sample      = channels == 1
			                              ? (frame[0] + frame[1]) * 0.5F
			                              : (channel < 2 ? frame[static_cast<std::size_t>(channel)] : 0.0F);
			output[index + channel] = sinkSample<Sample>(sample);
		}
		++m_outputFrame;
	}
}

void SoundPcmStream::render(QSpan<float> output)
{
	renderSamples(output);
}
void SoundPcmStream::render(QSpan<qint16> output)
{
	renderSamples(output);
}
void SoundPcmStream::render(QSpan<qint32> output)
{
	renderSamples(output);
}
void SoundPcmStream::render(QSpan<quint8> output)
{
	renderSamples(output);
}
#endif
