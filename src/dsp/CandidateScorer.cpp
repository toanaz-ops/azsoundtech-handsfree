#include "dsp/CandidateScorer.h"

#include <algorithm>
#include <cmath>

CandidateScorer::CandidateScorer() = default;

void CandidateScorer::setRiseReferenceMs (double ms)
{
    riseReferenceMs_.store (std::clamp (ms, kMinRiseReferenceMs, kMaxRiseReferenceMs),
                            std::memory_order_relaxed);
}

double CandidateScorer::getRiseReferenceMs() const
{
    return riseReferenceMs_.load (std::memory_order_relaxed);
}

void CandidateScorer::beginBlock (double sampleRate)
{
    sampleRate_ = sampleRate;

    // First-call sizing. baselineEma_ is value-initialised to zero anyway, but
    // fill it explicitly so the warm-up state does not depend on the
    // zero-initialisation of a std::array<double, 513> member surviving
    // refactors.
    if (! buffersReady_)
    {
        baselineEma_.fill (0.0);
        buffersReady_ = true;
    }
}

float CandidateScorer::scoreCandidate (const PeakinessAnalyzer::Candidate& candidate,
                                       const float* magnitudes,
                                       const LockedFrequencyView& lockedFrequencies)
{
    return scoreCandidateDetailed (candidate, magnitudes, lockedFrequencies).score;
}

CandidateScorer::ScoreBreakdown CandidateScorer::scoreCandidateDetailed (
    const PeakinessAnalyzer::Candidate& candidate,
    const float* magnitudes,
    const LockedFrequencyView& lockedFrequencies)
{
    ScoreBreakdown out;
    out.rawPeakiness = candidate.peakiness;

    // The scorer never resurrects a bin the analyzer already rejected: without
    // this gate, a candidate just under the threshold could still clear the
    // confirm score through the other two axes.
    if (candidate.peakiness <= PeakinessAnalyzer::kDefaultThreshold)
        return out;   // score 0, axes 0, penalty 1 -- product is 0 as before

    // Axis 1 -- peakiness. Saturates at 10x threshold: beyond that the bin is
    // peaky "enough" and extra sharpness must not substitute for novelty.
    float pNorm = (candidate.peakiness / PeakinessAnalyzer::kDefaultThreshold - 1.0f)
                  / 9.0f;
    pNorm = std::min (std::max (pNorm, 0.0f), 1.0f);
    out.pNorm = pNorm;

    // Axis 2 -- rise rate against the frame closest to ~riseReferenceMs ago.
    //
    // History semantics (plan KD-2, amended by the 2026-08-24 tuning brief):
    // a reference OLDER than 45% of the configured rise reference is real
    // history and is compared normally; frames younger than that are not deep
    // enough in time to claim a rise from. Three cases:
    //   * no history at all  -> everything is "rising" by definition (rNorm 1),
    //     otherwise a howl on the very first frames after startup would be
    //     invisible for its first half second;
    //   * history exists but every frame is younger than the minimum age ->
    //     we simply do not KNOW whether this rose, so rNorm 0 (conservative);
    //   * otherwise compare against the newest frame at least that old --
    //     newest-first scan, because commitBlock stamps monotonically
    //     increasing times, so the first hit IS the largest qualifying timeMs.
    float rNorm = 0.0f;
    if (historyCount_ == 0)
    {
        rNorm = 1.0f;
        out.riseRatio = 1.0f;   // lane G: nothing to compare against yet
    }
    else
    {
        const double minAgeMs = 0.45 * riseReferenceMs_.load (std::memory_order_relaxed);
        const float* reference = nullptr;
        for (std::size_t i = 0; i < historyCount_; ++i)
        {
            const std::size_t idx = (historyHead_ + kMaxHistoryFrames - 1 - i)
                                    % kMaxHistoryFrames;
            if (clockMs_ - history_[idx].timeMs >= minAgeMs)
            {
                reference = history_[idx].magnitudes.data();
                out.refFrame = reference;
                out.refAgeMs = clockMs_ - history_[idx].timeMs;
                break;
            }
        }

        if (reference != nullptr)
        {
            // Floor the divisor: a silent reference bin would make the ratio
            // inf (or NaN when both are zero), and NaN compares false against
            // every clamp bound -- it would poison the whole product silently.
            const float was  = std::max (reference[candidate.bin], 1e-12f);
            const float rise = magnitudes[candidate.bin] / was;
            out.riseRatio = rise;   // lane G: the raw ratio, unsaturated
            rNorm = (rise - 1.0f) / 0.5f;
            rNorm = std::min (std::max (rNorm, 0.0f), 1.0f);
        }
        // else: history too young -- rNorm stays 0 (see above).
    }
    out.rNorm = rNorm;

    // Axis 3 -- novelty against the per-bin EMA baseline. A tone that has been
    // there for seconds IS the new baseline and must stop scoring, even while
    // still perfectly peaky.
    const double mag      = static_cast<double> (magnitudes[candidate.bin]);
    const double baseline = std::max (baselineEma_[candidate.bin], 1e-12);
    double mNorm = std::log (std::max (mag, 1e-12) / baseline) / std::log (4.0);
    mNorm = std::min (std::max (mNorm, 0.0), 1.0);
    out.mNorm = static_cast<float> (mNorm);

    // Harmonic penalty (KD-3): a candidate sitting at roughly an integer
    // multiple (here 1.4x..4.1x) of a LOCKED notch frequency is likely a
    // harmonic of the already-notched fundamental, i.e. a symptom, not a
    // separate howl. Applied once regardless of how many fundamentals match.
    float penalty = 1.0f;
    if (lockedFrequencies.data != nullptr)
    {
        for (std::size_t i = 0; i < lockedFrequencies.count; ++i)
        {
            const double lockedHz = lockedFrequencies.data[i];
            if (lockedHz <= 0.0)
                continue;
            if (candidate.frequencyHz > 1.4 * lockedHz
                && candidate.frequencyHz < 4.1 * lockedHz)
            {
                penalty = kHarmonicPenalty;
                break;
            }
        }
    }
    out.penalty = penalty;

    // Product form (KD-4): all three axes must agree. Each axis alone has a
    // known false-positive mode (steady tones are peaky; broadband bursts are
    // novel); only a genuinely NEW narrow howl clears all three.
    out.score = out.pNorm * out.rNorm * out.mNorm * out.penalty;
    return out;
}

void CandidateScorer::commitBlock (const float* magnitudes, double elapsedMs)
{
    clockMs_ += elapsedMs;

    // Per-bin EMA with the pinned ~3 s time constant. Updated ONLY here, so
    // scoreCandidate() within a block reads state that is consistent for every
    // candidate of that block.
    if (elapsedMs > 0.0)
    {
        const double alpha = 1.0 - std::exp (-elapsedMs / kBaselineTimeConstantMs);
        for (int i = 0; i < kBins; ++i)
            baselineEma_[i] += alpha * (static_cast<double> (magnitudes[i])
                                        - baselineEma_[i]);
    }

    // Push this frame into the rise-history ring, overwriting the oldest entry
    // once full. kMaxHistoryFrames x ~10.7 ms covers the maximum rise
    // reference (1200 ms) with room to spare.
    auto& slot      = history_[historyHead_];
    slot.timeMs     = clockMs_;
    std::copy_n (magnitudes, kBins, slot.magnitudes.data());
    historyHead_    = (historyHead_ + 1) % kMaxHistoryFrames;
    if (historyCount_ < static_cast<std::size_t> (kMaxHistoryFrames))
        ++historyCount_;
}
