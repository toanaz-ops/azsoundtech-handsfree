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

// A 1 kHz tone at 48 kHz with the 2048-point FFT lands at
// bin = 1000 / (48000/2048) = 42.67, so bin 43 is the closest bin and must
// dominate. Analytic check of the same window/tone gives
// mag[43]/mean(all other bins) far above the >= 5x bound.
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

    constexpr int kToneBin = 43;   // 1000 / (48000/2048) = 42.67
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
//
// It must ALSO land in the right place. Asserting only on readCount leaves the
// slide itself untested: changing Detector.cpp to slide by the CONSTANT kHopSize
// instead of the variable readCount --
//   std::memmove (history_.data(), history_.data() + kHopSize, retained * ...)
// -- keeps every readCount assertion green while leaving the window misaligned.
// So this test feeds a RAMP, where the value of a sample identifies its
// position, and then checks the whole window through getAnalysisWindowForTest().
//
// Geometry, worked out by hand:
//   history_ starts as 1024 zeros.
//   Tap holds 712 samples with value i+1, i.e. 1.0 .. 712.0 (1-based so that a
//   real sample can never be confused with the zero prefix).
//   Block 1 reads a full hop of 512 -> retained = 512, window becomes
//     [0]*512  then 1..512            (positions 512..1023)
//   Block 2 reads the remaining 200  -> retained = 1024 - 200 = 824, so the
//   window slides left by 200 and the new samples go at [824..1023]:
//     [0]*312  then 1..712            (positions 312..1023)
//   i.e. 312 zeros followed by the ramp, contiguous and right-aligned.
//
// Under the mutation the second slide would move by 512 rather than 200, so
// history_[0] would be 1.0 instead of 0.0 and the check fails on the first
// element. Integers up to 2^24 are exact in float, so these are exact equalities.
TEST (Detector, PartialReadIsReportedAsPartial)
{
    constexpr std::size_t kPartial = 200;
    static_assert (kPartial < static_cast<std::size_t> (Detector::kHopSize),
                   "the point of this test is a short hop");

    constexpr std::size_t kTotal = static_cast<std::size_t> (Detector::kHopSize) + kPartial;  // 712

    LockFreeRingBuffer<float> tap (kTapCapacity);
    std::vector<float> ramp (kTotal);
    for (std::size_t i = 0; i < kTotal; ++i)
        ramp[i] = static_cast<float> (i + 1);
    ASSERT_EQ (tap.write (ramp.data(), ramp.size()), kTotal);

    Detector detector (48000.0);

    const auto full = detector.processLatestBlock (tap);
    ASSERT_NE (full.magnitudes, nullptr);
    ASSERT_EQ (full.readCount, static_cast<std::size_t> (Detector::kHopSize));

    const auto spectrum = detector.processLatestBlock (tap);

    EXPECT_NE (spectrum.magnitudes, nullptr);
    EXPECT_EQ (spectrum.readCount, kPartial);
    EXPECT_LT (spectrum.readCount, static_cast<std::size_t> (Detector::kHopSize));

    // The window slid by exactly the short hop, not by kHopSize.
    const float* window = detector.getAnalysisWindowForTest();
    ASSERT_NE (window, nullptr);

    constexpr std::size_t kZeroPrefix = static_cast<std::size_t> (Detector::kFftSize) - kTotal;  // 312
    for (std::size_t i = 0; i < kZeroPrefix; ++i)
        ASSERT_FLOAT_EQ (window[i], 0.0f) << "stale sample at history_[" << i << "]";

    for (std::size_t i = 0; i < kTotal; ++i)
        ASSERT_FLOAT_EQ (window[kZeroPrefix + i], static_cast<float> (i + 1))
            << "window not contiguous at history_[" << (kZeroPrefix + i) << "]";
}

// B2 -- reset() must zero the analysis window, so audio captured before a
// sample-rate change is never spliced into a spectrum labelled with the new
// rate. "Same as a freshly constructed detector fed the same hop" is the
// strongest statement of that, and it is bit-exact: both paths run the same
// window and the same FFT over the same 1024 floats.
TEST (Detector, ResetClearsAnalysisWindow)
{
    constexpr double sampleRate = 48000.0;

    // Reference: a brand-new detector fed exactly one hop of DC 1.0.
    LockFreeRingBuffer<float> referenceTap (kTapCapacity);
    const std::vector<float> oneHop (static_cast<std::size_t> (Detector::kHopSize), 1.0f);
    ASSERT_EQ (referenceTap.write (oneHop.data(), oneHop.size()), oneHop.size());

    Detector reference (sampleRate);
    const auto referenceSpectrum = reference.processLatestBlock (referenceTap);
    ASSERT_NE (referenceSpectrum.magnitudes, nullptr);
    const std::vector<float> expected (referenceSpectrum.magnitudes,
                                       referenceSpectrum.magnitudes + Detector::kNumBins);

    // Subject: prime the whole window with DC 1.0 (two full hops), reset, then
    // feed the same single hop.
    LockFreeRingBuffer<float> tap (kTapCapacity);
    const std::vector<float> primer (static_cast<std::size_t> (Detector::kFftSize), 1.0f);
    ASSERT_EQ (tap.write (primer.data(), primer.size()), primer.size());

    Detector detector (sampleRate);
    detector.processLatestBlock (tap);
    const auto primed = detector.processLatestBlock (tap);
    ASSERT_NE (primed.magnitudes, nullptr);

    // Sanity: a full window of DC really is a different spectrum from one hop
    // of DC, otherwise the comparison below would prove nothing.
    ASSERT_GT (std::abs (primed.magnitudes[0] - expected[0]), 1.0f);

    detector.reset();

    ASSERT_EQ (tap.write (oneHop.data(), oneHop.size()), oneHop.size());
    const auto afterReset = detector.processLatestBlock (tap);
    ASSERT_NE (afterReset.magnitudes, nullptr);
    ASSERT_EQ (afterReset.readCount, static_cast<std::size_t> (Detector::kHopSize));

    for (int bin = 0; bin < Detector::kNumBins; ++bin)
        ASSERT_FLOAT_EQ (afterReset.magnitudes[bin], expected[bin]) << "bin " << bin;
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
// analytically mag[1]/mag[0] = 0.5007 and mag[2]/mag[0] = 0.00033 (the ratio
// is size-independent for the symmetric Hann). So the 0.5 ratio is present if
// and only if Hann was applied.
TEST (Detector, HannWindowApplied)
{
    LockFreeRingBuffer<float> tap (kTapCapacity);
    const std::vector<float> dc (Detector::kFftSize, 1.0f);
    ASSERT_EQ (tap.write (dc.data(), dc.size()), static_cast<std::size_t> (Detector::kFftSize));

    Detector detector (48000.0);

    // kFftSize / kHopSize hops to fill the whole analysis window with DC.
    Detector::Spectrum spectrum {};
    for (int i = 0; i < Detector::kFftSize / Detector::kHopSize; ++i)
        spectrum = detector.processLatestBlock (tap);

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
