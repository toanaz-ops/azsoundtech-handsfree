// Tests for CandidateScorer (detection-policy plan, Tasks A / KD-1..KD-4).
//
// Every test drives the REAL Detector + PeakinessAnalyzer pipeline with real
// sine tones in noise (same helper pattern as test_peakiness.cpp), then runs
// one full CandidateScorer cycle per spectrum:
//     beginBlock -> analyse -> scoreCandidate (per candidate) -> commitBlock
// Simulated time advances via the elapsedMs argument of commitBlock
// (kFrameMs = 512/48000 s = 10.667 ms per hop); nothing sleeps.
//
// Bin geometry at 48 kHz (see test_peakiness.cpp for the full table):
//   1 kHz -> bin 21 (984.375 Hz), 2 kHz -> bin 43 (2015.625 Hz).

#include <gtest/gtest.h>

#include "dsp/CandidateScorer.h"
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
constexpr std::size_t kTapCapacity = 8192;
constexpr int    kHop        = Detector::kHopSize;                  // 512
constexpr double kFrameMs    = 1000.0 * kHop / kSampleRate;         // 10.667

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

// Largest peakiness over every bin with a full annulus -- the same pessimistic
// bound test_peakiness.cpp uses, kept here as a positive control.
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

// One Detector + PeakinessAnalyzer + CandidateScorer rig.
struct Rig
{
    LockFreeRingBuffer<float> tap { kTapCapacity };
    Detector          detector { kSampleRate };
    PeakinessAnalyzer analyzer;
    CandidateScorer   scorer;

    // Score/count bookkeeping from the most recent cycle().
    int      lastCandidateCount = 0;
    int      lastTopBin         = -1;
    double   lastTopFreqHz      = 0.0;
    float    lastWorstAnnulus   = 0.0f;

    // Feeds one hop of `signal` starting at `offset`, then runs one full
    // detect-score-commit cycle. Returns the highest candidate score this
    // frame (0 when there were no candidates).
    float cycle (const std::vector<float>& signal,
                 std::size_t offset,
                 const std::vector<double>& lockedHz = {})
    {
        EXPECT_EQ (tap.write (signal.data() + offset, static_cast<std::size_t> (kHop)),
                   static_cast<std::size_t> (kHop));

        const auto spectrum = detector.processLatestBlock (tap);
        scorer.beginBlock (kSampleRate);

        float best = 0.0f;
        lastCandidateCount = 0;

        if (spectrum.magnitudes != nullptr)
        {
            lastWorstAnnulus = maxPeakiness (spectrum.magnitudes, Detector::kNumBins);

            const CandidateScorer::LockedFrequencyView view { lockedHz.data(),
                                                              lockedHz.size() };
            const auto detected = analyzer.analyse (spectrum);
            lastCandidateCount  = static_cast<int> (detected.count);

            // Candidates are ordered by DESCENDING peakiness, so [0] is the
            // strongest peak regardless of what the scorer did with it -- the
            // right thing for existence/identity assertions.
            if (detected.count > 0)
            {
                lastTopBin    = detected.candidates[0].bin;
                lastTopFreqHz = detected.candidates[0].frequencyHz;
            }

            for (std::size_t i = 0; i < detected.count; ++i)
            {
                const float s = scorer.scoreCandidate (detected.candidates[i],
                                                       spectrum.magnitudes, view);
                if (s > best)
                {
                    best           = s;
                    lastTopBin     = detected.candidates[i].bin;
                    lastTopFreqHz  = detected.candidates[i].frequencyHz;
                }
            }

            scorer.commitBlock (spectrum.magnitudes, kFrameMs);
        }
        return best;
    }

    void feedHops (const std::vector<float>& signal, std::size_t numHops)
    {
        for (std::size_t h = 0; h < numHops; ++h)
            cycle (signal, h * static_cast<std::size_t> (kHop));
    }
};

constexpr std::size_t samplesFor (std::size_t hops) { return hops * static_cast<std::size_t> (kHop); }

} // namespace

// ---------------------------------------------------------------------------
// 1. Broadband noise alone must never confirm. Swept over ten seeds, with the
//    history fully warm first (~1 s) so the rise and novelty axes are live --
//    otherwise the test could pass merely because the history was too young to
//    score anything.
// ---------------------------------------------------------------------------
TEST (CandidateScorer, SilentNoiseNeverConfirms)
{
    for (unsigned seed : { 1u, 7u, 42u, 99u, 2024u, 12345u, 31337u, 65535u, 424242u, 999983u })
    {
        Rig rig;
        const auto noise = makeToneInNoise (0.0, 0.0f, 1.0f, seed, samplesFor (100));

        // ~1 s of noise: the history goes deeper than 450 ms, so both the rise
        // and novelty axes are live -- the assertion below cannot pass merely
        // because the scorer had no time depth to score anything.
        float maxScore = 0.0f;
        float worstAnnulusEver = 0.0f;
        for (std::size_t h = 0; h < 100; ++h)
        {
            maxScore = std::max (maxScore, rig.cycle (noise, h * static_cast<std::size_t> (kHop)));
            worstAnnulusEver = std::max (worstAnnulusEver, rig.lastWorstAnnulus);
        }

        // Positive control that the metric is alive: noise drives real
        // peakiness values. NOTE the upper bound from test_peakiness.cpp
        // (<9.0 on ONE primed spectrum) deliberately does NOT carry over:
        // across 100 frames per seed the extreme grows past the 10.0
        // threshold itself (measured up to 10.36 here) -- and THAT is exactly
        // the false-positive mode this scorer exists to reject, which the
        // score assertion below proves.
        EXPECT_GT (worstAnnulusEver, 2.0f) << "seed " << seed;

        // The requirement itself.
        EXPECT_LT (maxScore, CandidateScorer::kConfirmScore)
            << "seed " << seed << " max score " << maxScore;
    }
}

// ---------------------------------------------------------------------------
// 2. A strong howl appearing suddenly after ~2 s of quiet room noise must
//    clear the confirm score on its first blocks: peaky AND rising AND novel.
// ---------------------------------------------------------------------------
TEST (CandidateScorer, FreshHowlScoresAboveConfirm)
{
    Rig rig;
    constexpr unsigned kSeed = 12345u;

    // ~2 s of quiet noise warms the baseline EMA and the rise history.
    const auto quiet = makeToneInNoise (0.0, 0.0f, 0.05f, kSeed, samplesFor (190));
    rig.feedHops (quiet, 190);

    // The howl: strong 1 kHz tone, same noise floor, appearing suddenly.
    const auto howl = makeToneInNoise (1000.0, 1.0f, 0.05f, kSeed + 1, samplesFor (20));

    float bestEarly = 0.0f;
    for (int h = 0; h < 5; ++h)   // first ~53 ms of the howl
        bestEarly = std::max (bestEarly, rig.cycle (howl,
                                                    static_cast<std::size_t> (h) * static_cast<std::size_t> (kHop)));

    EXPECT_EQ (rig.lastTopBin, 21);              // 1 kHz lands on bin 21
    EXPECT_GT (bestEarly, CandidateScorer::kConfirmScore)
        << "best early score " << bestEarly;
}

// ---------------------------------------------------------------------------
// 3. The SAME tone held for ~30 s must stop confirming well before the end:
//    the baseline EMA catches up (novelty dies) and the rise settles at 1.0.
// ---------------------------------------------------------------------------
TEST (CandidateScorer, SteadyToneScoreDecays)
{
    Rig rig;
    constexpr unsigned kSeed = 12345u;

    const auto quiet = makeToneInNoise (0.0, 0.0f, 0.05f, kSeed, samplesFor (190));
    rig.feedHops (quiet, 190);

    // ~30 s of steady 1 kHz tone.
    constexpr std::size_t kToneHops = 2820;   // 30.08 s simulated
    const auto howl = makeToneInNoise (1000.0, 1.0f, 0.05f, kSeed + 1, samplesFor (kToneHops));

    std::size_t firstBelow = kToneHops;       // sentinel: never dropped
    float finalScore = 0.0f;
    bool earlyConfirmed = false;

    for (std::size_t h = 0; h < kToneHops; ++h)
    {
        const float s = rig.cycle (howl, h * static_cast<std::size_t> (kHop));
        if (s >= CandidateScorer::kConfirmScore && h < 10)
            earlyConfirmed = true;
        if (s < CandidateScorer::kConfirmScore && firstBelow == kToneHops)
            firstBelow = h;
        finalScore = s;
    }

    // It DID confirm early (positive control tying this to test 2)...
    EXPECT_TRUE (earlyConfirmed);
    // ...and it fell below confirmation WELL before the 30 s ran out...
    EXPECT_LT (firstBelow, kToneHops / 2)
        << "first cycle below confirm: " << firstBelow
        << " of " << kToneHops;
    // ...and stayed there at the end.
    EXPECT_LT (finalScore, CandidateScorer::kConfirmScore);
}

// ---------------------------------------------------------------------------
// 4. A candidate near 4x a LOCKED notch frequency is penalised exactly x0.5.
//    Two identical rigs (same seeds, same signals): the only difference is the
//    locked-frequency view. 500 Hz locked -> window (700, 2050) Hz contains
//    the ~2 kHz candidate (bin 42 = 1968.75 Hz or bin 43 = 2015.625 Hz).
// ---------------------------------------------------------------------------
TEST (CandidateScorer, HarmonicOfLockedNotchIsPenalised)
{
    constexpr unsigned kSeed = 4242u;
    constexpr std::size_t kWarmup = 120;   // ~1.3 s, history deep enough
    constexpr std::size_t kHowlHops = 10;

    const auto quiet = makeToneInNoise (0.0, 0.0f, 0.05f, kSeed, samplesFor (kWarmup));
    const auto howl  = makeToneInNoise (2000.0, 1.0f, 0.05f, kSeed + 1, samplesFor (kHowlHops));

    Rig cleanRig;
    cleanRig.feedHops (quiet, kWarmup);
    Rig lockedRig;
    lockedRig.feedHops (quiet, kWarmup);

    const std::vector<double> locked { 500.0 };

    float cleanMax = 0.0f;
    float lockedMax = 0.0f;
    for (std::size_t h = 0; h < kHowlHops; ++h)
    {
        cleanMax  = std::max (cleanMax,  cleanRig.cycle  (howl, h * static_cast<std::size_t> (kHop)));
        lockedMax = std::max (lockedMax, lockedRig.cycle (howl, h * static_cast<std::size_t> (kHop),
                                                          locked));
    }

    // The comparison must not be vacuous: the clean howl really confirmed, and
    // its top candidate really sits inside the penalty window.
    ASSERT_GT (cleanMax, CandidateScorer::kConfirmScore) << "clean max " << cleanMax;
    ASSERT_GT (lockedRig.lastTopFreqHz, 1.4 * 500.0);
    ASSERT_LT (lockedRig.lastTopFreqHz, 4.1 * 500.0);

    // Exactly half -- both rigs saw bit-identical inputs, so the only allowed
    // difference is the single multiply by kHarmonicPenalty.
    EXPECT_NEAR (lockedMax, cleanMax * CandidateScorer::kHarmonicPenalty, 1e-6f)
        << "clean " << cleanMax << " locked " << lockedMax;
}

// ---------------------------------------------------------------------------
// 5. With only 1-2 committed frames -- all younger than 450 ms -- there is no
//    time depth to claim a rise from, so even a textbook-strong howl scores 0.
//    (historyCount_ > 0 rules out the "nothing seen yet" exemption.)
// ---------------------------------------------------------------------------
TEST (CandidateScorer, YoungHistoryDoesNotClaimARise)
{
    Rig rig;
    constexpr unsigned kSeed = 777u;

    // A handful of noise frames: clock ends at ~53 ms, every history entry is
    // far younger than the 450 ms rise reference age. (More than two frames so
    // the analysis window holds real signal, not the startup silence prefix.)
    const auto quiet = makeToneInNoise (0.0, 0.0f, 0.05f, kSeed, samplesFor (5));
    rig.feedHops (quiet, 5);

    // The howl arrives. The window spans the LAST TWO consumed hops and lags
    // the block count by two, so the first two howl cycles still carry
    // noise/mixed windows -- a half-window tone smears across the annulus and
    // would not even be peaky. Score on the THIRD howl cycle, whose window is
    // fully howl; history is then all younger than ~100 ms.
    const auto howl = makeToneInNoise (1000.0, 1.0f, 0.05f, kSeed + 1, samplesFor (3));
    ASSERT_EQ (rig.cycle (howl, 0), 0.0f);   // sanity: not-yet-tonal window scores 0
    ASSERT_EQ (rig.cycle (howl, static_cast<std::size_t> (kHop)), 0.0f);
    const float score = rig.cycle (howl, 2 * static_cast<std::size_t> (kHop));

    // Positive control: the howl WAS seen as a peakiness candidate, so the
    // zero comes from the rise gate, not from an empty analysis.
    ASSERT_EQ (rig.lastTopBin, 21);
    EXPECT_FLOAT_EQ (score, 0.0f);
}
