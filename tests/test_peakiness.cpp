// Tests for peakiness scoring (Task 11).
//
// Most tests build a Detector::Spectrum BY HAND from a local std::vector<float>
// of Detector::kNumBins magnitudes. Spectrum is a plain struct holding a
// const float*, so no FFT and no ring buffer are needed, and the expected
// values are exact rather than approximate. The real Detector is used only
// where the test is genuinely about end-to-end behaviour.
//
// The metric under test is an ANNULUS: the neighbourhood is the SIX bins at
// offsets -5,-4,-3,+3,+4,+5. Offsets 0, +-1, +-2 are the Hann main lobe and
// are excluded (see PeakinessAnalyzer.h for the measurement that forced this).
// Two consequences drive the arithmetic below:
//   * adjacent bins are NOT each other's neighbours any more, so a two-bin
//     ridge no longer suppresses itself;
//   * a bin needs five bins of headroom on BOTH sides, so the lowest scoreable
//     bin is 5 -- 234.375 Hz at 48 kHz, ABOVE the 100 Hz product floor.
//
// Bin geometry at 48 kHz, kFftSize = 1024:  binWidth = 48000/1024 = 46.875 Hz
//   bin 4   = 187.500 Hz  -> below the annulus floor, never scored
//   bin 5   = 234.375 Hz  -> the lowest scoreable bin at this rate
//   bin 6   = 281.250 Hz  (exactly)
//   100 Hz  -> bin 2.1333 -> ceil = 3, but max(5, 3) = 5 -> the RADIUS binds
//   500 Hz  -> bin 10.667 -> ceil = 11 -> first scoreable bin is 11
//   1000 Hz -> bin 21.333 -> bin 21 = 984.375 Hz, bin 22 = 1031.25 Hz
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

// Largest peakiness over every bin with a full annulus, ignoring the
// local-maximum rule and the threshold. This is the pessimistic bound: it is
// >= what analyse() can ever see, because analyse() additionally requires a
// strict local maximum.
float maxPeakiness (const float* mags, int numBins)
{
    float best = 0.0f;
    for (int bin = PeakinessAnalyzer::kNeighbourOuterRadius;
         bin <= numBins - 1 - PeakinessAnalyzer::kNeighbourOuterRadius;
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
// 1. The metric itself, and the main-lobe exclusion that defines it.
//
// Bins all 1.0 except bin 10 = 5.0. The neighbourhood of bin 10 is the annulus
// {5, 6, 7, 13, 14, 15}, all 1.0, so
//     mean      = (1+1+1+1+1+1)/6 = 6.0/6 = 1.0     (exact in binary float)
//     peakiness = 5.0 / 1.0       = 5.0             (exact)
// ---------------------------------------------------------------------------
TEST (Peakiness, PeakinessAtUsesAnAnnulusThatExcludesTheMainLobe)
{
    auto mags = flatSpectrum (1.0f);
    mags[10]  = 5.0f;

    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 10),
                     5.0f);

    // The centre bin must NOT be part of its own neighbourhood. Had it been
    // included the mean would be (6*1 + 5)/7 = 11/7 = 1.5714 and peakiness
    // 5/1.5714 = 3.18.
    EXPECT_NE (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 10),
               5.0f * 7.0f / 11.0f);

    // *** The fix, pinned. *** Bins 8, 9, 11, 12 -- offsets +-1 and +-2 -- are
    // the Hann main lobe and must be IGNORED. Loading them up with the tone's
    // own energy must not move the answer by one ulp. Under the old solid-+-2
    // formula this same spectrum would have scored
    //     5.0 / ((3+3+3+3)/4) = 5.0/3.0 = 1.67   instead of 5.0.
    mags[8] = mags[9] = mags[11] = mags[12] = 3.0f;
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 10),
                     5.0f);

    // And the annulus bins DO count: raising all six to 2.0 must halve it.
    //     mean = (2+2+2+2+2+2)/6 = 2.0, peakiness = 5.0/2.0 = 2.5
    mags[5] = mags[6] = mags[7] = mags[13] = mags[14] = mags[15] = 2.0f;
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 10),
                     2.5f);
}

// ---------------------------------------------------------------------------
// 2. The plan's required pair, as ONE test so the separation is visible:
//    "1 kHz tone in white noise -> expect detection" and
//    "broadband noise only -> no false positive", both at the DEFAULT 10.0.
//
// This is the test that failed before the annulus fix. With the spec's solid
// +-2 neighbourhood the tone scored 3.29 and noise scored up to 3.46 -- noise
// beat the tone, and nothing could ever cross 10.0. Excluding the four-bin
// Hann main lobe restores the separation. MEASURED IN THIS RIG, seed 12345:
//
//     tone peakiness at bin 21            131.70
//     worst noise-only bin, same seed       4.51   (tone removed, same RNG)
//     threshold                            10.00
//
// i.e. the tone clears the threshold by 13.2x, the noise floor sits 2.2x
// BELOW it, and tone/noise separation is 29x. The bounds asserted here are
// deliberately loose enough to survive a different RNG but tight enough to
// fail if the annulus is ever narrowed back into the main lobe -- that would
// drag the tone under the old 4.0 ceiling, an order of magnitude below the
// lower bound.
// ---------------------------------------------------------------------------
TEST (Peakiness, ToneInNoiseIsDetectedAndNoiseAloneIsNot)
{
    LockFreeRingBuffer<float> tap (kTapCapacity);
    Detector detector (kSampleRate);

    const auto signal = makeToneInNoise (1000.0, 1.0f, 0.05f, 12345u,
                                         4 * static_cast<std::size_t> (Detector::kFftSize));
    const auto spectrum = primedSpectrum (detector, tap, signal);

    ASSERT_NE (spectrum.magnitudes, nullptr);
    ASSERT_EQ (spectrum.readCount, static_cast<std::size_t> (Detector::kHopSize));

    // 1000 Hz / 46.875 = bin 21.333, so the tone lands on bin 21 and is a
    // clean local maximum.
    constexpr int kToneBin = 21;
    EXPECT_GT (spectrum.magnitudes[kToneBin], spectrum.magnitudes[kToneBin - 1]);
    EXPECT_GE (spectrum.magnitudes[kToneBin], spectrum.magnitudes[kToneBin + 1]);

    // bin 21 * 48000 / 1024 = 984.375 Hz
    EXPECT_NEAR (kToneBin * kSampleRate / Detector::kFftSize, 984.375, 1.0);

    // MEASURED: 131.70 with this seed; 123..139 across five seeds in an
    // independent double-precision reference model. The lower bound of 40.0 is
    // 4x the threshold and 10x the old 4.0 ceiling -- narrowing the annulus
    // back into the main lobe cannot pass it.
    const float tonePeakiness =
        PeakinessAnalyzer::peakinessAt (spectrum.magnitudes, Detector::kNumBins, kToneBin);
    EXPECT_GT (tonePeakiness, 40.0f);
    EXPECT_LT (tonePeakiness, 400.0f);

    // The plan's requirement, at the plan's threshold, unmodified: exactly one
    // candidate, at the tone bin. The four-bin main lobe lights up bins 20, 22
    // and 23 as well, and bin 22 scores >10 in its own right -- the
    // local-maximum rule is what collapses them.
    PeakinessAnalyzer analyzer;
    ASSERT_FLOAT_EQ (analyzer.getThreshold(), 10.0f);

    const auto detected = analyzer.analyse (spectrum);
    ASSERT_EQ (detected.count, 1u);
    EXPECT_EQ (detected.candidates[0].bin, kToneBin);
    EXPECT_DOUBLE_EQ (detected.candidates[0].frequencyHz, 984.375);
    EXPECT_FLOAT_EQ (detected.candidates[0].peakiness, tonePeakiness);
    EXPECT_FLOAT_EQ (detected.candidates[0].score, PeakinessAnalyzer::kCandidateScore);

    // Same rig, same seed, same threshold, tone removed: nothing at all.
    LockFreeRingBuffer<float> noiseTap (kTapCapacity);
    Detector noiseDetector (kSampleRate);
    const auto noiseOnly = makeToneInNoise (0.0, 0.0f, 0.05f, 12345u,
                                            4 * static_cast<std::size_t> (Detector::kFftSize));
    const auto noiseSpectrum = primedSpectrum (noiseDetector, noiseTap, noiseOnly);
    ASSERT_NE (noiseSpectrum.magnitudes, nullptr);

    EXPECT_EQ (analyzer.analyse (noiseSpectrum).count, 0u);

    // The margin, asserted rather than asserted-about. Peakiness is
    // scale-invariant, so this ratio is the real discriminator and does not
    // depend on how loud the howl is.
    const float noiseWorst = maxPeakiness (noiseSpectrum.magnitudes, Detector::kNumBins);
    EXPECT_GT (tonePeakiness, 10.0f * noiseWorst) << "noise worst " << noiseWorst;
}

// ---------------------------------------------------------------------------
// 3. "Broadband noise only -> no false positive", swept over ten seeds.
//
// Unlike the pre-fix version of this test it does NOT hold vacuously: test 2
// proves the same analyzer at the same threshold does fire on a tone.
//
// maxPeakiness ignores the local-maximum rule, so it is a strict upper bound
// on what analyse() could report. MEASURED across these ten seeds: 3.19..5.14,
// comfortably under the 10.0 threshold, while a tone scores 131.70 (test 2).
//
// Widened to seeds 1..60 in this same rig, every seed still gave zero
// candidates and the worst bound was 7.35 (seed 59, next worst 7.23 on seed
// 14). That 7.35 is the honest headroom figure -- 1.36x under the threshold,
// not the 2x the ten-seed list alone suggests -- and it is why the bound below
// is 9.0 rather than something cosier. It is also why the +-3..+-4 annulus was
// rejected: in the reference model it crossed 10.0 outright on a noise-only
// frame.
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
        const float worst = maxPeakiness (spectrum.magnitudes, Detector::kNumBins);
        EXPECT_GT (worst, 2.0f) << "seed " << seed;

        // Teeth: the headroom under the threshold is what makes the metric
        // usable, so it is pinned rather than described.
        EXPECT_LT (worst, 9.0f) << "seed " << seed;
    }
}

// ---------------------------------------------------------------------------
// 4. Local-maximum rule collapses a two-bin ridge to one candidate.
//
// Under the annulus, bins 21 and 22 are NOT in each other's neighbourhood
// (offset 1 < kNeighbourInnerRadius), so a ridge no longer suppresses itself
// and both bins genuinely clear the DEFAULT threshold. That makes the
// local-maximum rule the only thing that can collapse them, which is exactly
// what this test needs to isolate.
//
//     background 1.0, mag[21] = 40.0, mag[22] = 30.0
//     annulus(21) = {16,17,18,24,25,26} = all 1.0  -> mean 1.0
//     annulus(22) = {17,18,19,25,26,27} = all 1.0  -> mean 1.0
//     peakiness(21) = 40 / 1.0 = 40.0  > 10.0  -> qualifies
//     peakiness(22) = 30 / 1.0 = 30.0  > 10.0  -> qualifies
//     local max 21: 40 > 1 and 40 >= 30  -> kept
//     local max 22: 30 > 40 is false     -> rejected
// (This is the ridge the original brief proposed. It did not work under the
// broken solid-+-2 formula -- both bins scored under 10 there -- and it works
// exactly as proposed now.)
// ---------------------------------------------------------------------------
TEST (Peakiness, AdjacentBinsCollapseToOneCandidate)
{
    auto mags = flatSpectrum (1.0f);
    mags[21]  = 40.0f;
    mags[22]  = 30.0f;

    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 21),
                     40.0f);
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (mags.data(), Detector::kNumBins, 22),
                     30.0f);

    PeakinessAnalyzer analyzer;
    ASSERT_FLOAT_EQ (analyzer.getThreshold(), 10.0f);

    const auto result = analyzer.analyse (wrap (mags));
    ASSERT_EQ (result.count, 1u);
    EXPECT_EQ (result.candidates[0].bin, 21);
    EXPECT_FLOAT_EQ (result.candidates[0].peakiness, 40.0f);

    // Exact plateau: two equal bins must still collapse to one, which is what
    // the asymmetric `>` on the left and `>=` on the right buys us.
    //     peakiness(21) = peakiness(22) = 40 / 1.0 = 40.0 > 10.0
    //     local max 21: 40 > 1 and 40 >= 40 -> kept
    //     local max 22: 40 > 40 is false    -> rejected
    mags[22] = 40.0f;
    const auto plateau = analyzer.analyse (wrap (mags));
    ASSERT_EQ (plateau.count, 1u);
    EXPECT_EQ (plateau.candidates[0].bin, 21);
}

// ---------------------------------------------------------------------------
// 5. The minimum-frequency knob, not merely its default.
//
// Spike at bin 6 = 6 * 46.875 = 281.25 Hz, height 40.0 on a flat 1.0 floor.
//     annulus(6) = {1, 2, 3, 9, 10, 11} = all 1.0 -> mean 1.0
//     peakiness(6) = 40 / 1.0 = 40.0  > 10.0
// With minFrequencyHz = 500: firstBin = max(5, ceil(500/46.875)) = max(5, 11) = 11,
//     so bin 6 is below the floor and must be skipped.
// With minFrequencyHz = 100: firstBin = max(5, ceil(100/46.875)) = max(5, 3) = 5,
//     so bin 6 is scored and becomes the candidate. Note that the OUTER RADIUS
//     wins here, not the 100 Hz setting -- see test 5b.
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

// ---------------------------------------------------------------------------
// 5b. *** THE KNOWN v1 LIMITATION, PINNED AS A TEST. ***
//
// kDefaultMinFrequencyHz is 100.0, but the annulus needs five bins of headroom
// on both sides, so nothing below bin 5 can ever be scored. At 48 kHz:
//     bin 4 = 187.500 Hz  -> ABOVE the 100 Hz product floor, still invisible
//     bin 5 = 234.375 Hz  -> the real low-frequency floor of the detector
// Low-mid feedback around 200-250 Hz is a real live-sound failure mode, so
// this gap is asserted rather than left to be discovered in a venue. If a
// future change (longer FFT, noise-floor tracker) closes it, this test must be
// updated deliberately -- it will not fail silently.
// ---------------------------------------------------------------------------
TEST (Peakiness, LowestScoreableBinIsSetByTheOuterRadiusNotMinFrequency)
{
    PeakinessAnalyzer analyzer;
    analyzer.setMinFrequencyHz (50.0);   // far below the default 100 Hz
    // firstBin = max(5, ceil(50/46.875)) = max(5, 2) = 5

    // A textbook 187.5 Hz howl, 40x the floor: invisible.
    auto low = flatSpectrum (1.0f);
    low[4]   = 40.0f;
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (low.data(), Detector::kNumBins, 4), 0.0f);
    EXPECT_EQ (analyzer.analyse (wrap (low)).count, 0u);

    // One bin higher -- 234.375 Hz -- and the same howl is detected.
    //     annulus(5) = {0, 1, 2, 8, 9, 10} = all 1.0 -> mean 1.0
    //     peakiness(5) = 40 / 1.0 = 40.0 > 10.0
    auto edge = flatSpectrum (1.0f);
    edge[5]   = 40.0f;
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (edge.data(), Detector::kNumBins, 5), 40.0f);
    const auto result = analyzer.analyse (wrap (edge));
    ASSERT_EQ (result.count, 1u);
    EXPECT_EQ (result.candidates[0].bin, 5);
    // 5 * 48000 / 1024 = 234.375 Hz
    EXPECT_DOUBLE_EQ (result.candidates[0].frequencyHz, 234.375);
    EXPECT_GT (result.candidates[0].frequencyHz, PeakinessAnalyzer::kDefaultMinFrequencyHz);
}

// The minimum bin must be DERIVED from spectrum.sampleRate, never hardcoded.
// At 96 kHz binWidth = 96000/1024 = 93.75 Hz, so 500 Hz -> ceil(5.333) = 6 and
// firstBin = max(5, 6) = 6; the same bin-6 spike that is excluded at 48 kHz
// (where firstBin = max(5, 11) = 11) is now exactly ON the floor and must be
// scored.
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
    //     annulus(100) = {95,96,97,103,104,105} = all 1.0 -> mean 1.0
    //     peakiness(100) = 40 / 1.0 = 40.0 > 10.0
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

    // An isolated spike on an otherwise silent floor is the actual 0/0 case:
    // the six annulus bins are all 0, so a naive ratio is 5.0/0.0 = +infinity.
    auto lone = flatSpectrum (0.0f);
    lone[100] = 5.0f;
    const float isolated = PeakinessAnalyzer::peakinessAt (lone.data(), Detector::kNumBins, 100);
    EXPECT_TRUE (std::isfinite (isolated));
    EXPECT_FLOAT_EQ (isolated, 0.0f);

    // Positive control: the guard must return 0 because the mean is 0, not
    // because peakinessAt always returns 0.
    //     annulus(100) = {95,96,97,103,104,105} = all 1.0 -> mean 1.0
    //     peakiness(100) = 40 / 1.0 = 40.0
    auto live = flatSpectrum (1.0f);
    live[100] = 40.0f;
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (live.data(), Detector::kNumBins, 100),
                     40.0f);
}

// ---------------------------------------------------------------------------
// 7b. Edge bins: the OUTER radius is what defines "has a full neighbourhood".
//
// Valid range is [5, kNumBins-1-5] = [5, 507] at kNumBins = 513. A live 1.0
// floor is used so a 0.0f return can only mean "out of range", never "the mean
// was zero".
// ---------------------------------------------------------------------------
TEST (Peakiness, BinsWithoutAFullAnnulusScoreZero)
{
    static_assert (PeakinessAnalyzer::kNeighbourOuterRadius == 5, "geometry below");
    static_assert (PeakinessAnalyzer::kNeighbourInnerRadius == 3, "geometry below");
    static_assert (PeakinessAnalyzer::kNeighbourCount == 6, "geometry below");
    static_assert (Detector::kNumBins == 513, "geometry below");

    const auto live = flatSpectrum (1.0f);
    const auto* m   = live.data();

    // Below the floor: bins 0..4 (and any negative index) have no lower half.
    for (int bin = -5; bin <= 4; ++bin)
        EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (m, Detector::kNumBins, bin), 0.0f)
            << "bin " << bin;

    // Above the ceiling: 513 - 1 - 5 = 507 is the last valid bin.
    for (int bin = 508; bin <= 513; ++bin)
        EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (m, Detector::kNumBins, bin), 0.0f)
            << "bin " << bin;

    // Both boundary bins ARE scored: flat 1.0 -> mean 1.0 -> peakiness 1.0.
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (m, Detector::kNumBins, 5), 1.0f);
    EXPECT_FLOAT_EQ (PeakinessAnalyzer::peakinessAt (m, Detector::kNumBins, 507), 1.0f);
}

// ---------------------------------------------------------------------------
// 8. Ordering: descending peakiness.
//
// Flat 1.0 with two spikes 50 bins apart, far outside each other's +-5
// annulus:
//     bin 50  = 30.0 -> annulus all 1.0 -> peakiness = 30.0
//     bin 100 = 20.0 -> annulus all 1.0 -> peakiness = 20.0
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
// kMaxCandidates + 4 = 36 spikes. Spacing is 6, not the old 5: with the +-3..+-5
// annulus a neighbour 5 bins away is still INSIDE the neighbourhood, so a
// 5-bin spacing would make every spike part of its neighbour's background and
// the expected peakiness would no longer equal the height. At spacing 6 the
// nearest other spike sits at offset +-6, outside the annulus, so
//     annulus(spike) = six bins of flat 1.0 -> mean 1.0
//     peakiness(spike) = height / 1.0 = height       (exact)
// Heights ascend with bin index: spike i sits at bin 100 + 6*i with height
// 20 + i, so the TALLEST (55.0) is at the HIGHEST bin, 100 + 6*35 = 310.
// An implementation that stops scanning after 32 hits would keep bins
// 100..286 (heights 20..51) and silently drop the strongest feedback.
// The 32 survivors must be heights 55 down to 24, i.e. bins 310 down to 124.
// ---------------------------------------------------------------------------
TEST (Peakiness, KeepsTheStrongestWhenOverCapacity)
{
    constexpr int kSpikes  = PeakinessAnalyzer::kMaxCandidates + 4;   // 36
    constexpr int kSpacing = 6;
    constexpr int kFirst   = 100;

    static_assert (kSpacing > PeakinessAnalyzer::kNeighbourOuterRadius,
                   "spikes must fall outside each other's annulus");

    auto mags = flatSpectrum (1.0f);
    for (int i = 0; i < kSpikes; ++i)
        mags[static_cast<std::size_t> (kFirst + kSpacing * i)] = 20.0f + static_cast<float> (i);

    PeakinessAnalyzer analyzer;
    const auto result = analyzer.analyse (wrap (mags));

    ASSERT_EQ (result.count, static_cast<std::size_t> (PeakinessAnalyzer::kMaxCandidates));

    // The tallest spike: bin 100 + 6*35 = 310, height 20 + 35 = 55.
    constexpr int kTallestBin = kFirst + kSpacing * (kSpikes - 1);
    static_assert (kTallestBin == 310, "arithmetic in the comment above");
    EXPECT_TRUE (containsBin (result, kTallestBin));
    EXPECT_EQ (result.candidates[0].bin, kTallestBin);
    EXPECT_FLOAT_EQ (result.candidates[0].peakiness, 55.0f);

    // The four shortest spikes (heights 20..23 at bins 100, 106, 112, 118) are
    // the ones that must have been dropped.
    for (int i = 0; i < 4; ++i)
        EXPECT_FALSE (containsBin (result, kFirst + kSpacing * i)) << "spike " << i;

    // Weakest survivor is height 24 at bin 100 + 6*4 = 124.
    EXPECT_FLOAT_EQ (result.candidates[result.count - 1].peakiness, 24.0f);
    EXPECT_EQ (result.candidates[result.count - 1].bin, 124);
}

// ---------------------------------------------------------------------------
// 10. The threshold is EXCLUSIVE: spec and plan both say `> threshold`.
//
// Flat 1.0 with a spike of exactly 10.0:
//     mean(annulus) = (1+1+1+1+1+1)/6 = 6.0/6 = 1.0   (exact in binary float)
//     peakiness     = 10.0 / 1.0      = 10.0          (exact in binary float)
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
