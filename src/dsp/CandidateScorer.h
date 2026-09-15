// CandidateScorer: turns raw spectral candidates into confidence scores.
//
// Consumes one spectrum frame per pump of the detector thread via
// beginBlock()/commitBlock(), and scores each PeakinessAnalyzer::Candidate
// against THREE independent axes (spec 5.2 steps 4-5); all three are
// saturated to [0,1] and MULTIPLIED -- a candidate must be peaky AND rising
// AND novel against its running baseline before it can confirm. The product
// form is deliberate: the product's headline goal is FEWER false positives,
// and each axis alone has known false-positive modes.
//
//   peakiness : bin_mag / mean(annulus +-3..+-5), threshold 10.0 (measured --
//               see PeakinessAnalyzer.h; do not retune without the 60-seed sweep)
//   rise      : mag_now / mag_~250ms_ago (runtime-tunable), threshold 1.5x
//   novelty   : log-ratio against a per-bin EMA baseline (~3 s time constant)
//   harmonic  : x0.5 when the candidate sits at 1.4x..4.1x of a LOCKED notch
//               (plan Task 12) -- harmonics of an already-notched fundamental
//               are symptoms, not separate howls
//
// Threading: designed to live entirely on the detector thread. Allocation
// happens only in beginBlock() on the first block (buffer sizing); after
// warm-up every path is allocation-free. NOT for the audio thread.

#pragma once

#include "dsp/PeakinessAnalyzer.h"

#include <array>
#include <atomic>
#include <cstddef>

class CandidateScorer
{
public:
    static constexpr double kBaselineTimeConstantMs = 3000.0;
    // Runtime-tunable (brief 2026-08-24): the rise reference is how far back
    // the scorer looks for its "was" magnitude; lower reacts faster. The
    // DEFAULT dropped from 500 ms to 250 ms (owner-approved tuning package).
    // Clamped on set; message thread writes, detector thread loads relaxed.
    static constexpr double kDefaultRiseReferenceMs = 250.0;
    static constexpr double kMinRiseReferenceMs     = 100.0;
    static constexpr double kMaxRiseReferenceMs     = 1000.0;
    // Must cover kMaxRiseReferenceMs plus the minimum-age margin:
    // kMaxHistoryFrames x ~10.7 ms ~= 1365 ms >= 1200 ms.
    static constexpr double kRiseHistoryWindowMs    = 1200.0;
    static constexpr double kRiseThreshold          = 1.5;
    static constexpr float  kHarmonicPenalty        = 0.5f;
    // A candidate whose peakiness does not clear the analyzer threshold is
    // scored 0 outright -- the scorer never resurrects a rejected bin.
    static constexpr float  kConfirmScore           = 0.7f;

    static constexpr int kBins = Detector::kNumBins;              // 1025
    static constexpr int kMaxHistoryFrames = 128;

    struct LockedFrequencyView
    {
        const double* data    = nullptr;
        std::size_t   count   = 0;
    };

    CandidateScorer();

    // Runtime rise reference (see constants above). Message thread; the
    // detector thread reads it relaxed in scoreCandidate().
    void   setRiseReferenceMs (double ms);
    double getRiseReferenceMs() const;

    // Call once per detector pump BEFORE scoring candidates. Sizes the
    // internal buffers on the first call (allocation-free afterwards).
    void beginBlock (double sampleRate);

    // Pure read against committed state: scores ONE candidate. Does not
    // mutate anything.
    float scoreCandidate (const PeakinessAnalyzer::Candidate& candidate,
                          const float* magnitudes,
                          const LockedFrequencyView& lockedFrequencies);

    // Lane D (data loop): the same computation, with every axis and the
    // reference frame the rise axis compared against exposed, so a session
    // log can record WHY a notch was placed. `refFrame` points into history_
    // and dies at the next commitBlock(). scoreCandidate() returns .score of
    // this and nothing else -- one arithmetic path, never two.
    struct ScoreBreakdown
    {
        float rawPeakiness = 0.0f;
        float pNorm = 0.0f, rNorm = 0.0f, mNorm = 0.0f;
        // Lane G (spec 4.2): the RAW rise ratio mag_now / mag_ref, before any
        // normalisation. rNorm above saturates at rise 1.5, so it cannot
        // distinguish a howl creeping up from one that jumped 12 dB in a
        // quarter second -- and that distinction is what decides whether a
        // notch starts at -6 or -12 dB. The neutral value is 1.0, meaning "no
        // measurable rise": the no-history branch, the history-too-young
        // branch, AND the sub-threshold early return (peakiness at or below
        // the analyzer threshold, CandidateScorer.cpp:52-53) all leave it at
        // this default, so an unknown rise can never buy a deeper starting
        // notch.
        // Unbounded above: against a near-silent reference the divisor floors
        // at 1e-12, so this can legitimately read ~1e12. Compare it against a
        // threshold; never scale anything by it.
        float riseRatio = 1.0f;
        float penalty = 1.0f;
        float score = 0.0f;
        const float* refFrame = nullptr;
        double refAgeMs = 0.0;
    };
    ScoreBreakdown scoreCandidateDetailed (const PeakinessAnalyzer::Candidate& candidate,
                                           const float* magnitudes,
                                           const LockedFrequencyView& lockedFrequencies);

    // Call once per detector pump AFTER all candidates are scored: advances
    // the EMA baselines and pushes this frame into the rise-history ring.
    void commitBlock (const float* magnitudes, double elapsedMs);

private:
    struct HistoryFrame
    {
        double timeMs = 0.0;
        std::array<float, kBins> magnitudes {};
    };

    double sampleRate_     = 48000.0;
    double clockMs_        = 0.0;
    bool   buffersReady_   = false;
    std::atomic<double>    riseReferenceMs_ { kDefaultRiseReferenceMs };

    std::array<double, kBins> baselineEma_ {};

    std::size_t            historyHead_  = 0;   // next write slot
    std::size_t            historyCount_ = 0;
    std::array<HistoryFrame, kMaxHistoryFrames> history_;
};
