// Tests for the detector FFT pipeline (Task 10).
//
// These tests drive a Detector directly from a locally-owned
// LockFreeRingBuffer -- they never touch AudioEngine, so nothing here needs
// an audio device.
//
// Expected magnitudes were derived analytically from the *symmetric* Hann
// window JUCE actually builds (juce_Windowing.cpp: w[i] = 0.5 - 0.5*cos(2*pi*i/(N-1)),
// then normalised so sum(w) == N). See the individual tests for the numbers.

#include <gtest/gtest.h>

#include "dsp/Detector.h"
#include "dsp/LockFreeRingBuffer.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;

// Capacity matching AudioEngine::kTapCapacity, so the tests exercise the
// detector against the same ring-buffer geometry the audio thread uses.
constexpr std::size_t kTapCapacity = 8192;

std::vector<float> makeSine (double frequencyHz, double sampleRate, std::size_t numSamples)
{
    std::vector<float> out (numSamples);
    for (std::size_t i = 0; i < numSamples; ++i)
        out[i] = static_cast<float> (std::sin (2.0 * kPi * frequencyHz
                                               * static_cast<double> (i) / sampleRate));
    return out;
}
} // namespace

// A 1 kHz tone at 48 kHz with a 1024-point FFT lands at
// bin = 1000 / (48000/1024) = 21.33, so bin 21 is the closest bin and must
// dominate. Analytic check of the same window/tone gives
// mag[21]/mean(all other bins) ~= 404, so the >= 5x bound has huge headroom.
TEST (Detector, MagnitudeOfSingleTone)
{
    constexpr double sampleRate = 48000.0;
    LockFreeRingBuffer<float> tap (kTapCapacity);

    const auto sine = makeSine (1000.0, sampleRate, 4 * Detector::kFftSize);
    ASSERT_EQ (tap.write (sine.data(), sine.size()), sine.size());

    Detector detector (sampleRate);

    // 4096 samples / 512-sample hop = 8 blocks; the history is fully primed
    // with real signal from block 2 onwards.
    Detector::Spectrum spectrum {};
    for (int i = 0; i < 8; ++i)
        spectrum = detector.processLatestBlock (tap);

    ASSERT_NE (spectrum.magnitudes, nullptr);
    ASSERT_EQ (spectrum.readCount, static_cast<std::size_t> (Detector::kHopSize));
    EXPECT_DOUBLE_EQ (spectrum.sampleRate, sampleRate);

    constexpr int kToneBin = 21;
    double sumOfOthers = 0.0;
    for (int bin = 0; bin < Detector::kNumBins; ++bin)
        if (bin != kToneBin)
            sumOfOthers += spectrum.magnitudes[bin];

    const double meanOfOthers = sumOfOthers / static_cast<double> (Detector::kNumBins - 1);
    EXPECT_GT (spectrum.magnitudes[kToneBin], 5.0 * meanOfOthers);
}

// A hop shorter than kHopSize means the analysis timeline advanced by less
// than one hop. That MUST surface in readCount rather than being padded up to
// a full hop: Task 14's auto-release cadence assumes a hop is real elapsed
// audio.
TEST (Detector, PartialReadIsReportedAsPartial)
{
    constexpr std::size_t kPartial = 200;
    static_assert (kPartial < static_cast<std::size_t> (Detector::kHopSize),
                   "the point of this test is a short hop");

    LockFreeRingBuffer<float> tap (kTapCapacity);
    const std::vector<float> block (kPartial, 0.5f);
    ASSERT_EQ (tap.write (block.data(), block.size()), kPartial);

    Detector detector (48000.0);
    const auto spectrum = detector.processLatestBlock (tap);

    EXPECT_NE (spectrum.magnitudes, nullptr);
    EXPECT_EQ (spectrum.readCount, kPartial);
    EXPECT_LT (spectrum.readCount, static_cast<std::size_t> (Detector::kHopSize));
}

// With nothing in the tap there is no new audio to analyse, so no spectrum is
// produced -- the caller must not be handed a stale pointer dressed up as new.
TEST (Detector, EmptyTapProducesNoSpectrum)
{
    LockFreeRingBuffer<float> tap (kTapCapacity);
    Detector detector (48000.0);

    const auto spectrum = detector.processLatestBlock (tap);

    EXPECT_EQ (spectrum.magnitudes, nullptr);
    EXPECT_EQ (spectrum.readCount, 0u);
}

TEST (Detector, SetSampleRateUpdates)
{
    Detector detector (48000.0);
    EXPECT_DOUBLE_EQ (detector.getSampleRate(), 48000.0);

    detector.setSampleRate (96000.0);
    EXPECT_DOUBLE_EQ (detector.getSampleRate(), 96000.0);
}

// Hann-vs-rectangular discriminator on a DC input.
//
// A rectangular window puts a DC signal entirely in bin 0: mag[1] == 0.
// JUCE's normalised symmetric Hann spreads it in a fixed, known way --
// analytically mag[1]/mag[0] = 0.5007 and mag[2]/mag[0] = 0.00033 for
// N = 1024. So the 0.5 ratio is present if and only if Hann was applied.
TEST (Detector, HannWindowApplied)
{
    LockFreeRingBuffer<float> tap (kTapCapacity);
    const std::vector<float> dc (Detector::kFftSize, 1.0f);
    ASSERT_EQ (tap.write (dc.data(), dc.size()), static_cast<std::size_t> (Detector::kFftSize));

    Detector detector (48000.0);

    // Two hops of 512 to fill the 1024-sample analysis window with DC.
    detector.processLatestBlock (tap);
    const auto spectrum = detector.processLatestBlock (tap);

    ASSERT_NE (spectrum.magnitudes, nullptr);
    ASSERT_EQ (spectrum.readCount, static_cast<std::size_t> (Detector::kHopSize));
    ASSERT_GT (spectrum.magnitudes[0], 0.0f);

    EXPECT_NEAR (spectrum.magnitudes[1] / spectrum.magnitudes[0], 0.5f, 0.01f);
    EXPECT_LT (spectrum.magnitudes[2], 0.01f * spectrum.magnitudes[0]);
}

// Mirrors NotchChain::setSampleRate: a non-positive rate is nonsense (it would
// make every bin-to-Hz conversion in Tasks 11-14 divide by zero or go
// negative), so it is ignored rather than stored.
TEST (Detector, SetSampleRateIgnoresNonPositive)
{
    Detector detector (48000.0);

    detector.setSampleRate (0.0);
    EXPECT_DOUBLE_EQ (detector.getSampleRate(), 48000.0);

    detector.setSampleRate (-44100.0);
    EXPECT_DOUBLE_EQ (detector.getSampleRate(), 48000.0);
}
