/**
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * @file tst_SoundPcmStream.cpp
 * @brief Verifies audible controls and streaming boundaries with a deterministic sample clock.
 */

#include "SoundPcmStream.h"
#include <QtTest/QTest>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
	/**
	 * @brief Creates a floating-point PCM format without opening an audio device.
	 * @param rate Sample rate.
	 * @param channels Channel count.
	 * @return Valid PCM format.
	 */
	QAudioFormat format(const int rate = 8000, const int channels = 2)
	{
		QAudioFormat result;
		result.setSampleRate(rate);
		result.setChannelCount(channels);
		result.setSampleFormat(QAudioFormat::Float);
		return result;
	}
	/**
	 * @brief Creates asymmetric constant stereo input for gain and balance assertions.
	 * @param frames Number of input frames.
	 * @param rate Source sample rate.
	 * @return PCM whose left and right samples are 0.8 and 0.4 respectively.
	 */
	QAudioBuffer constantBuffer(const int frames, const int rate = 8000)
	{
		QAudioBuffer result(frames, format(rate));
		auto        *data = result.data<float>();
		for (int index = 0; index < frames; ++index)
		{
			data[index * 2]     = 0.8F;
			data[index * 2 + 1] = 0.4F;
		}
		return result;
	}
	/** @brief Sample-clock tests independent of audio services and wall-clock timing. */
	class tst_SoundPcmStream : public QObject
	{
			Q_OBJECT
		private slots:
			/** @brief Verifies invalid initial controls retain neutral defaults and cannot poison PCM rendering. */
			static void nonFiniteControlsPreserveValidTransportState()
			{
				for (const double invalid :
				     {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
				      -std::numeric_limits<double>::infinity()})
				{
					SoundPcmStream stream(format(1000), invalid, invalid, invalid);
					QVERIFY(stream.append(constantBuffer(1000, 1000)));
					QCOMPARE(stream.state().volume, 1.0);
					QCOMPARE(stream.state().pan, 0.0);
					QCOMPARE(stream.state().pitch, 0.0);
					QCOMPARE(stream.state().playbackRate, 1.0);
					std::array<float, 4> output{};
					stream.render(QSpan<float>(output));
					QCOMPARE(output, (std::array<float, 4>{0.8F, 0.4F, 0.8F, 0.4F}));
					stream.setAttribute(QMudSoundAttribute::Volume, 0.0, std::chrono::milliseconds{100},
					                    true);
					stream.setAttribute(QMudSoundAttribute::Pan, 50.0, {});
					stream.setAttribute(QMudSoundAttribute::Pitch, 2.0, {});
					stream.setAttribute(QMudSoundAttribute::Volume, invalid, {});
					stream.setAttribute(QMudSoundAttribute::Pan, invalid, {});
					stream.setAttribute(QMudSoundAttribute::Pitch, invalid, {});
					stream.setGlobalVolume(invalid);
					QCOMPARE(stream.state().pan, 50.0);
					QCOMPARE(stream.state().pitch, 2.0);
					std::array<float, 200> faded{};
					stream.render(QSpan<float>(faded));
					QVERIFY(stream.state().faded);
					QCOMPARE(stream.state().volume, 0.0);
					QVERIFY(
					    std::ranges::all_of(faded, [](const float sample) { return std::isfinite(sample); }));
				}
				for (const double pitch :
				     {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()})
				{
					SoundPcmStream stream(format(1000), 0.5, 25.0, pitch);
					QVERIFY(stream.append(constantBuffer(1000, 1000)));
					QCOMPARE(stream.state().pitch, pitch);
					QCOMPARE(stream.state().playbackRate, pitch > 0.0 ? 16.0 : 0.01);
					std::array<float, 4> output{};
					stream.render(QSpan<float>(output));
					QCOMPARE(output, (std::array<float, 4>{0.3F, 0.2F, 0.3F, 0.2F}));
				}
			}
			/** @brief Verifies no-op global volume preserves fades, including at their start and midpoint. */
			static void globalVolumePreservesUnchangedEnvelopes()
			{
				SoundPcmStream stream(format(1000), 1.0, 0.0, 0.0);
				QVERIFY(stream.append(constantBuffer(4000, 1000)));
				stream.setAttribute(QMudSoundAttribute::Volume, 0.0, std::chrono::milliseconds{1000}, true);
				stream.setGlobalVolume(1.0);
				std::vector<float> output(1000);
				stream.render(QSpan<float>(output));
				QCOMPARE(stream.state().volume, 0.5);
				stream.setGlobalVolume(0.5);
				stream.render(QSpan<float>(output));
				QVERIFY(stream.state().faded);
				stream.setGlobalVolume(1.0);
				QCOMPARE(stream.state().volume, 0.0);
				QVERIFY(stream.state().faded);
				stream.setAttribute(QMudSoundAttribute::Volume, 1.0, {});
				stream.setAttribute(QMudSoundAttribute::Volume, 0.0, std::chrono::milliseconds{1000}, true);
				stream.render(QSpan<float>(output));
				stream.setGlobalVolume(0.75);
				stream.render(QSpan<float>(output));
				QCOMPARE(stream.state().volume, 0.75);
				QVERIFY(!stream.state().faded);
			}
			/** @brief Verifies equivalent binary gains retain fades for every integer percentage and fractional ramps. */
			static void equivalentGainsPreserveFadeCompletion()
			{
				for (int percent = 1; percent <= 100; ++percent)
				{
					const double   legacyGain = 1.0 + (static_cast<double>(percent) - 100.0) / 100.0;
					const double   requested  = static_cast<double>(percent) / 100.0;
					SoundPcmStream stream(format(1000), legacyGain, 0.0, 0.0);
					QVERIFY(stream.append(constantBuffer(4000, 1000)));
					stream.setAttribute(QMudSoundAttribute::Volume, 0.0, std::chrono::milliseconds{1000},
					                    true);
					stream.setGlobalVolume(requested);
					std::vector<float> output(1000);
					stream.render(QSpan<float>(output));
					stream.setGlobalVolume(requested * 0.5);
					stream.render(QSpan<float>(output));
					QVERIFY2(stream.state().faded, qPrintable(QString::number(percent)));
					QCOMPARE(stream.state().volume, 0.0);
				}
				for (const double initial : {0.00125, 0.3425, 0.45, 0.99999, 1.0e-14})
				{
					SoundPcmStream stream(format(1000), initial, 0.0, 0.0);
					QVERIFY(stream.append(constantBuffer(4000, 1000)));
					stream.setAttribute(QMudSoundAttribute::Volume, 1.0, std::chrono::milliseconds{1000});
					std::vector<float> partial(500);
					stream.render(QSpan<float>(partial));
					const double interrupted = stream.state().volume;
					stream.setAttribute(QMudSoundAttribute::Volume, 0.0, std::chrono::milliseconds{1000},
					                    true);
					stream.setGlobalVolume(std::nextafter(interrupted, 1.0));
					std::vector<float> output(2000);
					stream.render(QSpan<float>(output));
					QVERIFY(stream.state().faded);
					stream.setGlobalVolume(initial);
					QVERIFY(stream.state().faded);
					QCOMPARE(stream.state().volume, 0.0);
				}
				// A quiet positive gain is still audible and must accept a real global change.
				SoundPcmStream quiet(format(1000), 1.0e-14, 0.0, 0.0);
				quiet.setGlobalVolume(2.0e-14);
				QVERIFY(quiet.state().volume == 2.0e-14);
				SoundPcmStream muted(format(1000), 0.0, 0.0, 0.0);
				QVERIFY(muted.append(constantBuffer(1000, 1000)));
				muted.setAttribute(QMudSoundAttribute::Volume, 1.0, std::chrono::milliseconds{1000});
				muted.setGlobalVolume(0.25);
				std::vector<float> output(2000);
				muted.render(QSpan<float>(output));
				QCOMPARE(muted.state().volume, 1.0);
			}

			/** @brief Verifies reopening decoder input retains queued samples, resampling phase, and all control envelopes. */
			static void reopenedInputPreservesQueuedAudioAndControls()
			{
				SoundPcmStream continued(format(2000), 0.5, 0.0, 0.0);
				SoundPcmStream reference(format(2000), 0.5, 0.0, 0.0);
				for (SoundPcmStream *stream : {&continued, &reference})
				{
					QVERIFY(stream->append(constantBuffer(1000, 1000)));
					stream->setAttribute(QMudSoundAttribute::Volume, 0.0, std::chrono::milliseconds{500},
					                     true);
					stream->setAttribute(QMudSoundAttribute::Pan, 80.0, std::chrono::milliseconds{500});
					stream->setAttribute(QMudSoundAttribute::Pitch, 2.0, std::chrono::milliseconds{500});
					std::array<float, 2> first{};
					stream->render(QSpan<float>(first));
				}
				const auto before = continued.state();
				continued.finish();
				continued.reopenInput();
				QCOMPARE(continued.state().renderedFrames, before.renderedFrames);
				QCOMPARE(continued.state().lastOutputFrame, before.lastOutputFrame);
				QCOMPARE(continued.state().volume, before.volume);
				QCOMPARE(continued.state().pan, before.pan);
				QCOMPARE(continued.state().pitch, before.pitch);
				std::vector<float> actual(2000);
				std::vector<float> expected(2000);
				continued.render(QSpan<float>(actual));
				reference.render(QSpan<float>(expected));
				QVERIFY(actual == expected);
				QVERIFY(continued.state().faded);
				continued.reopenInput();
				QVERIFY(continued.state().faded);
				SoundPcmStream drained(format(1000), 1.0, 0.0, 0.0);
				QVERIFY(drained.append(constantBuffer(1, 1000)));
				drained.finish();
				std::array<float, 2> output{};
				drained.render(QSpan<float>(output));
				QVERIFY(drained.state().drained);
				drained.reopenInput();
				QVERIFY(!drained.state().drained);
				QVERIFY(drained.append(constantBuffer(1, 1000)));
				drained.render(QSpan<float>(output));
				QCOMPARE(output[0], 0.8F);
				QCOMPARE(drained.state().renderedFrames, 2);
			}

			/** @brief Verifies pitch slides offset the current frequency, including an interrupted ramp. */
			static void pitchSlidesUseCurrentSampledFrequency()
			{
				SoundPcmStream stream(format(1000), 1.0, 0.0, 0.0);
				QVERIFY(stream.append(constantBuffer(4000, 1000)));
				stream.setAttribute(QMudSoundAttribute::Pitch, 5.0, {});
				stream.setAttribute(QMudSoundAttribute::Pitch, 2.0, std::chrono::milliseconds{1000}, false,
				                    QMudSoundAttributeTarget::Relative);
				std::vector<float> output(1000);
				stream.render(QSpan<float>(output));
				QCOMPARE(stream.state().pitch, 6.0);
				QCOMPARE(stream.state().playbackRate, 1.6);
				stream.setAttribute(QMudSoundAttribute::Pitch, -3.0, std::chrono::milliseconds{1000}, false,
				                    QMudSoundAttributeTarget::Relative);
				stream.render(QSpan<float>(output));
				QCOMPARE(stream.state().pitch, 4.5);
				stream.setAttribute(QMudSoundAttribute::Pitch, 2.0, {}, false,
				                    QMudSoundAttributeTarget::Relative);
				QCOMPARE(stream.state().pitch, 6.5);
			}
			/** @brief Verifies controls change PCM rather than only registry values. */
			static void gainAndBalanceAffectBothChannels()
			{
				SoundPcmStream stream(format(), 0.5, 100.0, 0.0);
				QVERIFY(stream.append(constantBuffer(100)));
				std::array<float, 4> output{};
				stream.render(QSpan<float>(output));
				QCOMPARE(output[0], 0.0F);
				QCOMPARE(output[1], 0.2F);
				stream.setAttribute(QMudSoundAttribute::Pan, -100.0, {});
				stream.render(QSpan<float>(output));
				QCOMPARE(output[0], 0.4F);
				QCOMPARE(output[1], 0.0F);
			}
			/** @brief Verifies independent continuous ramps, midpoint queries, replacement, and fade completion. */
			static void envelopesUseTheSampleClock()
			{
				SoundPcmStream stream(format(1000), 1.0, 0.0, 0.0);
				QVERIFY(stream.append(constantBuffer(3000, 1000)));
				stream.setAttribute(QMudSoundAttribute::Volume, 0.0, std::chrono::milliseconds{1000}, true);
				stream.setAttribute(QMudSoundAttribute::Pan, 100.0, std::chrono::milliseconds{1000});
				std::vector<float> output(1000);
				stream.render(QSpan<float>(output));
				QCOMPARE(stream.state().volume, 0.5);
				QCOMPARE(stream.state().pan, 50.0);
				QVERIFY(!stream.state().faded);
				QVERIFY(qAbs(output[500] - 0.45F) < 0.00001F);
				stream.setAttribute(QMudSoundAttribute::Volume, 1.0, std::chrono::milliseconds{500});
				stream.render(QSpan<float>(output));
				QCOMPARE(stream.state().volume, 1.0);
				QVERIFY(!stream.state().faded);
				stream.setAttribute(QMudSoundAttribute::Volume, 0.0, std::chrono::milliseconds{500}, true);
				stream.render(QSpan<float>(output));
				QVERIFY(stream.state().faded);
				stream.render(QSpan<float>(output));
				QVERIFY(std::ranges::all_of(output, [](const float value) { return value == 0.0F; }));
			}
			/** @brief Verifies legacy pitch offsets change source frequency by 100 Hz per unit. */
			static void frequencyAndSampleRateConversion()
			{
				QAudioBuffer input(100, format(8000, 1));
				for (int index = 0; index < 100; ++index)
					input.data<float>()[index] = static_cast<float>(index) / 100.0F;
				SoundPcmStream stream(format(16000), 1.0, 0.0, 0.0);
				QVERIFY(stream.append(input));
				std::array<float, 4> output{};
				stream.render(QSpan<float>(output));
				QCOMPARE(output[0], 0.0F);
				QVERIFY(qAbs(output[2] - 0.005F) < 0.000001F);
				stream.setAttribute(QMudSoundAttribute::Pitch, 80.0, {});
				QCOMPARE(stream.state().playbackRate, 2.0);
				stream.render(QSpan<float>(output));
				QVERIFY(qAbs(output[0] - 0.01F) < 0.000001F);
				QVERIFY(qAbs(output[2] - 0.02F) < 0.000001F);
				SoundPcmStream chunked(format(8000), 1.0, 0.0, 80.0);
				QAudioBuffer   chunk(3, format(8000, 1));
				chunk.data<float>()[0] = 0.0F;
				chunk.data<float>()[1] = 0.01F;
				chunk.data<float>()[2] = 0.02F;
				QVERIFY(chunked.append(chunk));
				chunked.render(QSpan<float>(output));
				QVERIFY(qAbs(output[2] - 0.02F) < 0.000001F);
				chunk.data<float>()[0] = 0.03F;
				chunk.data<float>()[1] = 0.04F;
				chunk.data<float>()[2] = 0.05F;
				QVERIFY(chunked.append(chunk));
				chunked.render(QSpan<float>(output));
				QVERIFY(qAbs(output[0] - 0.04F) < 0.000001F);
			}
			/** @brief Verifies decoder boundaries preserve interpolation and drain the final frame. */
			static void partialInputAndBoundedTransport()
			{
				SoundPcmStream stream(format(16000), 1.0, 0.0, 0.0);
				QVERIFY(stream.append(constantBuffer(1)));
				std::array<float, 2> output{};
				stream.render(QSpan<float>(output));
				QCOMPARE(output[0], 0.8F);
				stream.render(QSpan<float>(output));
				QCOMPARE(output[0], 0.0F);
				QVERIFY(stream.append(constantBuffer(1)));
				stream.render(QSpan<float>(output));
				QCOMPARE(output[0], 0.8F);
				stream.finish();
				stream.render(QSpan<float>(output));
				QCOMPARE(output[0], 0.8F);
				stream.render(QSpan<float>(output));
				QVERIFY(stream.state().drained);
				const auto finalFrame = stream.state().lastOutputFrame;
				stream.render(QSpan<float>(output));
				QCOMPARE(stream.state().lastOutputFrame, finalFrame);
				QVERIFY(stream.state().renderedFrames > finalFrame);
				SoundPcmStream bounded(format(), 1.0, 0.0, 0.0);
				QVERIFY(!bounded.append(constantBuffer(32'001)));
				QVERIFY(bounded.append(constantBuffer(32'000)));
				QVERIFY(!bounded.append(constantBuffer(1)));
			}
			/** @brief Verifies each supported sink representation uses the same gain and pan processing. */
			static void integerSinksAndFreshAssignments()
			{
				SoundPcmStream stream(format(), 0.5, -100.0, 0.0);
				QVERIFY(stream.append(constantBuffer(100)));
				std::array<qint16, 2> pcm16{};
				std::array<qint32, 2> pcm32{};
				std::array<quint8, 2> pcm8{};
				stream.render(QSpan<qint16>(pcm16));
				stream.render(QSpan<qint32>(pcm32));
				stream.render(QSpan<quint8>(pcm8));
				QCOMPARE(pcm16[0], 13107);
				QCOMPARE(pcm16[1], 0);
				QCOMPARE(pcm8[1], 128);
				QVERIFY(pcm32[0] > 800'000'000);
				QCOMPARE(pcm32[1], 0);
				SoundPcmStream fresh(format(), 1.0, 0.0, 0.0);
				QCOMPARE(fresh.state().pan, 0.0);
				QCOMPARE(fresh.state().pitch, 0.0);
				QCOMPARE(fresh.state().volume, 1.0);
			}
	};
} // namespace
QTEST_GUILESS_MAIN(tst_SoundPcmStream)
#include "tst_SoundPcmStream.moc"
