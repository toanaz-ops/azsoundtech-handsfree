// Tests for peakiness scoring (Task 11).
//
// Most tests build a Detector::Spectrum BY HAND from a local std::vector<float>
// of Detector::kNumBins magnitudes. Spectrum is a plain struct holding a
// const float*, so no FFT and no ring buffer are needed, and the expected
// values are exact rather than approximate. The real Detector is used only
// where the test is genuinely about end-to-end behaviour.
//
// Bin geometry at 48 kHz, kFftSize = 1024:  binWidth = 48000/1024 = 46.875 Hz
//   100 Hz  -> bin 2.1333  -> ceil = 3   -> first scoreable bin is 3
//   281.25  -> bin 6       (exactly)
//   500 Hz  -> bin 10.6667 -> ceil = 11  -> first scoreable bin is 11
//   1000 Hz -> bin 21.3333 -> bin 21 = 984.375 Hz, bin 22 = 1031.25 Hz
//
// Every numeric assertion below has its arithmetic written out above it.

#include <gtest/gtest.h>

#include "dsp/Detector.h"
#include "dsp/LockFreeRingBuffer.h"
#include "dsp/PeakinessAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

namespace
{
constexpr double kPi         = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;

// Matches AudioEngine::kTapCapacity, so the end-to-end tests exercise the same
// ring-buffer geometry the audio thread uses.
constexpr std::size_t kTapCapacity = 8192;

// A flat spectrum of `background` with nothing else in it.
std::vector<float> flatSpectrum (float background = 1.0f)
{
    return std::vector<float> (static_cast<std::size_t> (Detector::kNumBins), background);
}

Detector::Spectrum wrap (const std::vector<float>& mags, double sampleRate = kSampleRate)
{
    Detector::Spectrum s {};
    s.magnitudes = mags.data();
    s.readCount  = static_cast<std::size_t> (Detector::kHopSize);
    s.sampleRate = sampleRate;
    return s;
}

// Drives the real Detector from a tap filled with `signal`, returning the
// spectrum after the analysis window is fully primed with real signal.
// 4 * kFftSize samples / kHopSize = 8 blocks; after 8 blocks the history holds
// samples [3*kFftSize, 4*kFftSize).
Detector::Spectrum primedSpectrum (Detector& detector,
                                   LockFreeRingBuffer<float>& tap,
                                   const std::vector<float>& signal)
{
    EXPECT_EQ (tap.write (signal.data(), signal.size()), signal.size());

    Detector::Spectrum spectrum {};
    for (int i = 0; i < 8; ++i)
        spectrum = detector.processLatestBlock (tap);
    return spectrum;
}

std::vector<float> makeToneInNoise (double frequencyHz,
                                    float  toneAmplitude,
                                    float  noiseAmplitude,
                                    unsigned seed,
                                    std::size_t numSamples)
{
    // Fixed seed: a test that fails one run in fifty is worse than no test.
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> noise (-noiseAmplitude, noiseAmplitude);

    std::vector<float> out (numSamples);
    for (std::size_t i = 0; i < numSamples; ++i)
    {
        const float tone = toneAmplitude
                         * static_cast<float> (std::sin (2.0 * kPi * frequencyHz
                                                         * static_cast<double> (i) / kSampleRate));
        out[i] = tone + noise (rng);
    }
    return out;
}

// Largest peakiness over every bin with a full neighbourhood, ignoring the
// local-maximum rule and the threshold.
float maxPeakiness (const float* mags, int numBins)
{
    float best = 0.0f;
    for (int bin = PeakinessAnalyzer::kNeighbourRadius;
         bin <= numBins - 1 - PeakinessAnalyzer::kNeighbourRadius;
         ++bin)
    {
        best = std::max (best, PeakinessAnalyzer::peakinessAt (mags, numBins, bin));
    }
    return best;
}

bool containsBin (const PeakinessAnalyzer::Result& r, int bin)
{
    for (std::size_t i = 0; i < r.count; ++i)
        if (r.candidates[i].bin == bin)
            return true;
    return false;
}
} // namespace

// ---------------------------------------------------------------------------
// 1. The spec formula itself.
//
// Bins all 1.0 except bin 10 = 5.0. Neighbours of bin 10 are bins 8, 9, 11, 12,
// all 1.0, so mean = (1+1+1+1)/4 = 1.0 exactly and peakiness = 5.0/1.0 = 5.0.
// Every value here is exactly representable in binary float, so this is an
// exact equality, not an approximation.
// ---------------------------------------------------------------------------
TEST (Peakiness, PeakinessAtMatchesSpecFormula)
{
    auto mags = flatSpectrum (1.0f);
    mags[10]  = 5.0f;

    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 10),
                     5.0f);

    // The centre bin must NOT be part of its own neighbourhood. Had it been
    // included the mean would be (1+1+5+1+1)/5 = 1.8 and peakiness 5/1.8 = 2.78.
    EXPECT_NE (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 10),
               5.0f / 1.8f);
}

// ---------------------------------------------------------------------------
// 2. Plan's required test: "1 kHz tone in white noise -> expect detection".
//
// *** THIS TEST DOCUMENTS A DEFECT RATHER THAN A PASSING REQUIREMENT. ***
//
// The brief asks this test to assert exactly one candidate at bin 21 with
// peakiness > 10.0f. That is arithmetically impossible, and the assertions
// below record why, with the measured numbers.
//
// Detector applies a Hann window before a 1024-point FFT. A sinusoid therefore
// occupies a FOUR-bin main lobe, so bins 19, 20, 22 and 23 -- exactly the
// +-1/+-2 neighbourhood -- are filled by the tone itself and scale with it.
// Periodic-Hann kernel magnitudes at offsets (0, +-1, +-2) are (0.5, 0.25, 0):
//     mean(neighbours) = (0.25 + 0.25 + 0 + 0)/4 = 0.125, relative to 0.5
//     peakiness        = 0.5 / 0.125 = 4.0    <-- hard ceiling for ANY tone
//
// Independently computed magnitudes for this exact rig (JUCE's normalised
// symmetric Hann, tone at bin 21.333):
//     mag[19]=13.47  mag[20]=136.29  mag[21]=476.04  mag[22]=381.29  mag[23]=47.64
//     mean(19,20,22,23) = (13.47+136.29+381.29+47.64)/4 = 578.69/4 = 144.67
//     peakiness(21)     = 476.04 / 144.67 = 3.29
//
// Peakiness is scale-invariant, so a howl 40 dB louder still scores 3.29.
// Pure broadband noise scores up to 3.21 (see test 3), i.e. the tone is NOT
// separable from the noise floor at this radius. kDefaultThreshold = 10.0 can
// never fire, so `analyse` returns nothing for a textbook feedback tone.
// ---------------------------------------------------------------------------
TEST (Peakiness, ToneInNoiseIsNotDetectedBecauseTheHannMainLobeCapsPeakinessAtFour)
{
    LockFreeRingBuffer<float> tap (kTapCapacity);
    Detector detector (kSampleRate);

    const auto signal = makeToneInNoise (1000.0, 1.0f, 0.05f, 12345u,
                                         4 * static_cast<std::size_t> (Detector::kFftSize));
    const auto spectrum = primedSpectrum (detector, tap, signal);

    ASSERT_NE (spectrum.magnitudes, nullptr);
    ASSERT_EQ (spectrum.readCount, static_cast<std::size_t> (Detector::kHopSize));

    // The tone lands on bin 21 and IS a clean local maximum -- the FFT stage
    // and the local-max rule are both doing their job.
    constexpr int kToneBin = 21;
    EXPECT_GT (spectrum.magnitudes[kToneBin], spectrum.magnitudes[kToneBin - 1]);
    EXPECT_GE (spectrum.magnitudes[kToneBin], spectrum.magnitudes[kToneBin + 1]);

    // bin 21 * 48000 / 1024 = 984.375 Hz
    EXPECT_NEAR (kToneBin * kSampleRate / Detector::kFftSize, 984.375, 1.0);

    // Measured 3.283..3.301 across five RNG seeds; the theoretical ceiling is
    // 4.0 and the worst half-bin case is 2.80.
    const float tonePeakiness =
        PeakinessAnalyzer::peakinessAt (spectrum.magnitudes, Detector::kNumBins, kToneBin);
    EXPECT_GT (tonePeakiness, 3.2f);
    EXPECT_LT (tonePeakiness, 4.0f);

    // The defect, asserted so it cannot be forgotten: with the specified
    // threshold the plan's "expect detection" does not happen. When the
    // neighbourhood is widened past the main lobe this line MUST start failing.
    PeakinessAnalyzer analyzer;
    ASSERT_FLOAT_EQ (analyzer.getThreshold(), 10.0f);
    EXPECT_EQ (analyzer.analyse (spectrum).count, 0u);

    // Lowering the threshold below the ceiling does surface the tone bin, which
    // confirms the selection machinery is correct and the radius is the problem.
    analyzer.setThreshold (3.2f);
    EXPECT_TRUE (containsBin (analyzer.analyse (spectrum), kToneBin));
}

// ---------------------------------------------------------------------------
// 3. Plan's required test: "broadband noise only -> no false positive".
//
// Holds, but note it holds VACUOUSLY at the default threshold: nothing at all
// clears 10.0 (see test 2). The extra bound below gives the test teeth by
// pinning how high noise actually gets -- measured max peakiness over eight
// seeds was 2.80..3.21, which overlaps the 3.29 a real tone scores.
// ---------------------------------------------------------------------------
TEST (Peakiness, BroadbandNoiseProducesNoCandidate)
{
    for (unsigned seed : { 1u, 7u, 42u, 99u, 2024u, 12345u, 31337u, 65535u, 424242u, 999983u })
    {
        LockFreeRingBuffer<float> tap (kTapCapacity);
        Detector detector (kSampleRate);

        const auto signal = makeToneInNoise (0.0, 0.0f, 1.0f, seed,
                                             4 * static_cast<std::size_t> (Detector::kFftSize));
        const auto spectrum = primedSpectrum (detector, tap, signal);
        ASSERT_NE (spectrum.magnitudes, nullptr);

        PeakinessAnalyzer analyzer;
        EXPECT_EQ (analyzer.analyse (spectrum).count, 0u) << "seed " << seed;

        // Positive control: without this the test would pass against a
        // peakinessAt that always returns 0, i.e. it would prove nothing.
        // Measured noise maxima across eight seeds were 2.80..3.21.
        const float worst = maxPeakiness (spectrum.magnitudes, Detector::kNumBins);
        EXPECT_GT (worst, 2.0f) << "seed " << seed;

        // Noise never reaches the 4.0 Hann ceiling, but it gets uncomfortably
        // close to the 3.29 a genuine tone scores.
        EXPECT_LT (worst, 4.0f) << "seed " << seed;
    }
}

// ---------------------------------------------------------------------------
// 4. Local-maximum rule collapses a two-bin ridge to one candidate.
//
// The brief proposes flat 1.0 with bin 21 = 40.0 and bin 22 = 30.0, claiming
// "both bins clear the threshold". They do not -- adjacent bins are each
// other's neighbours:
//     peakiness(21) = 40 / ((1 + 1 + 30 + 1)/4) = 40 / 8.25  = 4.85   (< 10)
//     peakiness(22) = 30 / ((1 + 40 + 1 + 1)/4) = 30 / 10.75 = 2.79   (< 10)
// so that spectrum yields ZERO candidates and proves nothing about local maxima.
//
// In fact NO two-bin ridge can put both bins over 10. With background e,
//     peakiness(21) * peakiness(22) = [4a/(b+3e)] * [4b/(a+3e)] -> 16 as e -> 0,
// so the two values cannot both exceed 4.0. The ridge is therefore built here
// with a threshold of 2.0, chosen so BOTH bins clear it and the local-max rule
// is the only thing that can suppress bin 22:
//     a = mag[21] = 12.0, b = mag[22] = 11.0, background 1.0
//     peakiness(21) = 4*12/(11+3) = 48/14 = 3.4286  > 2.0  -> qualifies
//     peakiness(22) = 4*11/(12+3) = 44/15 = 2.9333  > 2.0  -> qualifies
//     local max 21: 12 > 1 and 12 >= 11  -> kept
//     local max 22: 11 > 12 is false     -> rejected
// ---------------------------------------------------------------------------
TEST (Peakiness, AdjacentBinsCollapseToOneCandidate)
{
    auto mags = flatSpectrum (1.0f);
    mags[21]  = 12.0f;
    mags[22]  = 11.0f;

    EXPECT_NEAR (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 21),
                 48.0f / 14.0f, 1e-5f);
    EXPECT_NEAR (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 22),
                 44.0f / 15.0f, 1e-5f);

    PeakinessAnalyzer analyzer;
    analyzer.setThreshold (2.0f);

    const auto result = analyzer.analyse (wrap (mags));
    ASSERT_EQ (result.count, 1u);
    EXPECT_EQ (result.candidates[0].bin, 21);

    // Exact plateau: two equal bins must still collapse to one, which is what
    // the asymmetric `>` on the left and `>=` on the right buys us.
    //     peakiness(21) = peakiness(22) = 4*12/(12+3) = 48/15 = 3.2 > 2.0
    //     local max 21: 12 > 1 and 12 >= 12 -> kept
    //     local max 22: 12 > 12 is false    -> rejected
    mags[22] = 12.0f;
    const auto plateau = analyzer.analyse (wrap (mags));
    ASSERT_EQ (plateau.count, 1u);
    EXPECT_EQ (plateau.candidates[0].bin, 21);
}

// ---------------------------------------------------------------------------
// 5. The minimum-frequency knob, not merely its default.
//
// Spike at bin 6 = 6 * 46.875 = 281.25 Hz, height 40.0 on a flat 1.0 floor:
//     peakiness(6) = 40 / ((1+1+1+1)/4) = 40 / 1.0 = 40.0  > 10.0
// With minFrequencyHz = 500: first bin = max(2, ceil(500/46.875)) = max(2, 11) = 11,
//     so bin 6 is below the floor and must be skipped.
// With minFrequencyHz = 100: first bin = max(2, ceil(100/46.875)) = max(2, 3) = 3,
//     so bin 6 is scored and becomes the candidate.
// ---------------------------------------------------------------------------
TEST (Peakiness, BinsBelowMinFrequencyAreIgnored)
{
    auto mags = flatSpectrum (1.0f);
    mags[6]   = 40.0f;

    PeakinessAnalyzer analyzer;
    analyzer.setMinFrequencyHz (500.0);
    EXPECT_DOUBLE_EQ (analyzer.getMinFrequencyHz(), 500.0);
    EXPECT_EQ (analyzer.analyse (wrap (mags)).count, 0u);

    analyzer.setMinFrequencyHz (100.0);
    const auto result = analyzer.analyse (wrap (mags));
    ASSERT_EQ (result.count, 1u);
    EXPECT_EQ (result.candidates[0].bin, 6);
    EXPECT_FLOAT_EQ (result.candidates[0].peakiness, 40.0f);
    EXPECT_FLOAT_EQ (result.candidates[0].magnitude, 40.0f);
    EXPECT_FLOAT_EQ (result.candidates[0].score, PeakinessAnalyzer::kCandidateScore);
    // 6 * 48000 / 1024 = 281.25 Hz
    EXPECT_DOUBLE_EQ (result.candidates[0].frequencyHz, 281.25);
}

// The minimum bin must be DERIVED from spectrum.sampleRate, never hardcoded.
// At 96 kHz binWidth = 96000/1024 = 93.75 Hz, so 500 Hz -> ceil(5.333) = bin 6,
// and the same bin-6 spike that is excluded at 48 kHz is now exactly ON the
// floor and must be scored.
TEST (Peakiness, MinimumBinFollowsTheSpectrumSampleRate)
{
    auto mags = flatSpectrum (1.0f);
    mags[6]   = 40.0f;

    PeakinessAnalyzer analyzer;
    analyzer.setMinFrequencyHz (500.0);

    EXPECT_EQ (analyzer.analyse (wrap (mags, 48000.0)).count, 0u);

    const auto at96k = analyzer.analyse (wrap (mags, 96000.0));
    ASSERT_EQ (at96k.count, 1u);
    EXPECT_EQ (at96k.candidates[0].bin, 6);
    // 6 * 96000 / 1024 = 562.5 Hz
    EXPECT_DOUBLE_EQ (at96k.candidates[0].frequencyHz, 562.5);
}

// ---------------------------------------------------------------------------
// 6. Detector hands back magnitudes == nullptr when the tap had no new audio.
//    That must not be dereferenced. A non-positive sample rate is equally
//    unusable, since every bin-to-Hz conversion would divide by it.
// ---------------------------------------------------------------------------
TEST (Peakiness, NullSpectrumProducesNoCandidate)
{
    PeakinessAnalyzer analyzer;

    const Detector::Spectrum empty {};
    ASSERT_EQ (empty.magnitudes, nullptr);

    const auto result = analyzer.analyse (empty);
    EXPECT_EQ (result.candidates, nullptr);
    EXPECT_EQ (result.count, 0u);

    auto mags = flatSpectrum (1.0f);
    mags[100] = 40.0f;
    EXPECT_EQ (analyzer.analyse (wrap (mags, 0.0)).count, 0u);
    EXPECT_EQ (analyzer.analyse (wrap (mags, -48000.0)).count, 0u);

    EXPECT_EQ (PeakinessAnalyzer::peakinessAt (nullptr, Detector::kNumBins, 100), 0.0f);

    // Positive control: the SAME magnitudes at a valid rate must yield the
    // candidate. Without this the test would pass against an analyse() that
    // unconditionally returns nothing.
    //     peakiness(100) = 40 / ((1+1+1+1)/4) = 40.0 > 10.0
    const auto valid = analyzer.analyse (wrap (mags, 48000.0));
    ASSERT_EQ (valid.count, 1u);
    EXPECT_EQ (valid.candidates[0].bin, 100);
}

// ---------------------------------------------------------------------------
// 7. Silence is an all-zero spectrum, so the neighbourhood mean is 0 and the
//    naive ratio is 0/0 = NaN. NaN compares false against every threshold, so
//    it would not LOOK broken while poisoning any later arithmetic that touches
//    it. peakinessAt must return a real number.
// ---------------------------------------------------------------------------
TEST (Peakiness, SilenceProducesNoCandidateAndNoNaN)
{
    const auto mags = flatSpectrum (0.0f);

    PeakinessAnalyzer analyzer;
    EXPECT_EQ (analyzer.analyse (wrap (mags)).count, 0u);

    const float mid = PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 100);
    EXPECT_TRUE (std::isfinite (mid));
    EXPECT_FLOAT_EQ (mid, 0.0f);

    // Bins without a full neighbourhood are out of scope for the metric.
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 1), 0.0f);
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins,
                                                     Detector::kNumBins - 1), 0.0f);
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, -5), 0.0f);

    // An isolated spike on an otherwise silent floor is the actual 0/0 case:
    // the four neighbours are all 0, so a naive ratio is 5.0/0.0 = +infinity.
    auto lone = flatSpectrum (0.0f);
    lone[100] = 5.0f;
    const float isolated = PeakinessAnalyzer::peakinessAt (lone.data(), Detector::kNumBins, 100);
    EXPECT_TRUE (std::isfinite (isolated));
    EXPECT_FLOAT_EQ (isolated, 0.0f);

    // Positive control: the guard must return 0 because the mean is 0, not
    // because peakinessAt always returns 0.
    //     peakiness(100) = 40 / ((1+1+1+1)/4) = 40.0
    auto live = flatSpectrum (1.0f);
    live[100] = 40.0f;
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (live.data(), Detector::kNumBins, 100),
                     40.0f);
}

// ---------------------------------------------------------------------------
// 8. Ordering: descending peakiness.
//
// Flat 1.0 with two well-separated spikes so neither sits in the other's
// neighbourhood:
//     bin 50  = 30.0 -> peakiness = 30 / ((1+1+1+1)/4) = 30.0
//     bin 100 = 20.0 -> peakiness = 20 / ((1+1+1+1)/4) = 20.0
// Both clear 10.0, and the taller one must come first.
// ---------------------------------------------------------------------------
TEST (Peakiness, CandidatesAreOrderedByDescendingPeakiness)
{
    auto mags = flatSpectrum (1.0f);
    mags[50]  = 30.0f;
    mags[100] = 20.0f;

    PeakinessAnalyzer analyzer;
    const auto result = analyzer.analyse (wrap (mags));

    ASSERT_EQ (result.count, 2u);
    EXPECT_EQ (result.candidates[0].bin, 50);
    EXPECT_EQ (result.candidates[1].bin, 100);
    EXPECT_FLOAT_EQ (result.candidates[0].peakiness, 30.0f);
    EXPECT_FLOAT_EQ (result.candidates[1].peakiness, 20.0f);
    EXPECT_GT (result.candidates[0].peakiness, result.candidates[1].peakiness);
}

// ---------------------------------------------------------------------------
// 9. Over capacity, keep the STRONGEST, not the first scanned.
//
// kMaxCandidates + 4 = 36 spikes, spaced 5 bins apart starting at bin 100 so
// that no spike falls inside another's +-2 neighbourhood (gap of 4 empty bins).
// Every spike's four neighbours are the flat 1.0 floor, so
//     peakiness(spike) = height / ((1+1+1+1)/4) = height / 1.0 = height.
// Heights ascend with bin index: spike i sits at bin 100 + 5*i with height
// 20 + i, so the TALLEST (55.0) is at the HIGHEST bin, 100 + 5*35 = 275.
// An implementation that stops scanning after 32 hits would keep bins
// 100..255 (heights 20..51) and silently drop the strongest feedback.
// The 32 survivors must be heights 55 down to 24, i.e. bins 275 down to 120.
// ---------------------------------------------------------------------------
TEST (Peakiness, KeepsTheStrongestWhenOverCapacity)
{
    constexpr int kSpikes  = PeakinessAnalyzer::kMaxCandidates + 4;   // 36
    constexpr int kSpacing = 5;
    constexpr int kFirst   = 100;

    auto mags = flatSpectrum (1.0f);
    for (int i = 0; i < kSpikes; ++i)
        mags[static_cast<std::size_t> (kFirst + kSpacing * i)] = 20.0f + static_cast<float> (i);

    PeakinessAnalyzer analyzer;
    const auto result = analyzer.analyse (wrap (mags));

    ASSERT_EQ (result.count, static_cast<std::size_t> (PeakinessAnalyzer::kMaxCandidates));

    // The tallest spike: bin 100 + 5*35 = 275, height 20 + 35 = 55.
    constexpr int kTallestBin = kFirst + kSpacing * (kSpikes - 1);
    static_assert (kTallestBin == 275, "arithmetic in the comment above");
    EXPECT_TRUE (containsBin (result, kTallestBin));
    EXPECT_EQ (result.candidates[0].bin, kTallestBin);
    EXPECT_FLOAT_EQ (result.candidates[0].peakiness, 55.0f);

    // The four shortest spikes (heights 20..23 at bins 100, 105, 110, 115) are
    // the ones that must have been dropped.
    for (int i = 0; i < 4; ++i)
        EXPECT_FALSE (containsBin (result, kFirst + kSpacing * i)) << "spike " << i;

    // Weakest survivor is height 24 at bin 120.
    EXPECT_FLOAT_EQ (result.candidates[result.count - 1].peakiness, 24.0f);
}

// ---------------------------------------------------------------------------
// 10. The threshold is EXCLUSIVE: spec and plan both say `> threshold`.
//
// Flat 1.0 with a spike of exactly 10.0:
//     mean(neighbours) = (1+1+1+1)/4 = 4.0/4 = 1.0   (exact in binary float)
//     peakiness        = 10.0 / 1.0   = 10.0         (exact in binary float)
// 10.0f > 10.0f is false, so the bin must NOT be a candidate. Nudging the
// spike up by one ulp-ish step must flip it, proving the boundary is where it
// is claimed to be and not merely that everything is being rejected.
// ---------------------------------------------------------------------------
TEST (Peakiness, ThresholdIsExclusive)
{
    auto mags = flatSpectrum (1.0f);
    mags[200] = 10.0f;

    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 200),
                     10.0f);

    PeakinessAnalyzer analyzer;
    ASSERT_FLOAT_EQ (analyzer.getThreshold(), 10.0f);
    EXPECT_EQ (analyzer.analyse (wrap (mags)).count, 0u);

    // peakiness = 10.001 / 1.0 = 10.001 > 10.0 -> now a candidate.
    mags[200] = 10.001f;
    const auto result = analyzer.analyse (wrap (mags));
    ASSERT_EQ (result.count, 1u);
    EXPECT_EQ (result.candidates[0].bin, 200);
}
