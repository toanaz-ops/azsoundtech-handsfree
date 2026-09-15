// src/dsp/SoundcheckCandidates.cpp
//
// The rule this file implements is written out in full at the head of
// SoundcheckCandidates.h. Every comparison below carries the unit of both
// sides, because the one thing that must never go wrong here is a sign.
#include "dsp/SoundcheckCandidates.h"

#include "dsp/SoundcheckSignal.h"

#include <algorithm>
#include <cmath>

SoundcheckCandidates::Depth
SoundcheckCandidates::depthFor (double hDb, double ceilingDb, const Ladder& ladder)
{
    Depth out;

    // Both sides dB. needed_dB > 0 means "this many dB must come out".
    const double needed = hDb + kTargetMarginDb;
    if (needed < kMinUsefulCutDb)
        return out;                        // MARK only; depthDb stays 0 and means "no proposal"

    const double depthRaw = -needed;       // <= -kMinUsefulCutDb, so strictly negative

    // The SHALLOWEST rung at least as deep as depthRaw. Deeper == more negative,
    // so "at least as deep" is r <= depthRaw, and "shallowest" is the largest
    // such r. Getting this backwards is spec rev 2's N2 defect.
    double rung  = ladder.maxDepthDb;
    bool   found = false;
    for (int i = 0; i < ladder.count; ++i)
    {
        const double r = ladder.rungsDb[i];
        if (r <= depthRaw && (! found || r > rung))
        {
            rung  = r;
            found = true;
        }
    }
    if (! found)
        rung = ladder.maxDepthDb;          // nothing deep enough -> saturate at the floor

    double depth = std::max (rung, ceilingDb);            // a SHALLOWER ceiling wins (Q13)
    depth        = std::max (depth, ladder.maxDepthDb);   // lane G invariant 1

    out.depthDb    = depth;
    // Both terms dB: what was needed, minus what the cut actually removes.
    out.residualDb = std::max (0.0, needed - (-depth));
    out.saturated  = out.residualDb > 0.0;
    return out;
}

void SoundcheckCandidates::smooth (const float* hDb, float* out, int numBins, double sampleRate)
{
    if (hDb == nullptr || out == nullptr || numBins <= 0 || ! (sampleRate > 0.0))
        return;

    // One third of an octave, centred on the bin: [f / 2^(1/6), f * 2^(1/6)].
    // A resonance is narrow against this window and a loudspeaker's roll-off is
    // not, which is exactly the discrimination step 5 needs.
    static const double kSixthOctave = std::pow (2.0, 1.0 / 6.0);

    for (int k = 0; k < numBins; ++k)
    {
        const double hz = LoopGainEstimator::binToHz (k, sampleRate);

        int lo = LoopGainEstimator::hzToBin (hz / kSixthOctave, sampleRate);
        int hi = LoopGainEstimator::hzToBin (hz * kSixthOctave, sampleRate);

        // The centre bin is always inside its own window, whatever the rounding
        // at the edges did, and the window never leaves the array.
        lo = std::max (0, std::min (lo, k));
        hi = std::min (numBins - 1, std::max (hi, k));

        double sum = 0.0;
        for (int j = lo; j <= hi; ++j)
            sum += (double) hDb[(std::size_t) j];

        out[(std::size_t) k] = (float) (sum / (double) (hi - lo + 1));
    }
}

SoundcheckCandidates::Output SoundcheckCandidates::pick (const Input& in)
{
    Output out;

    if (in.hDb == nullptr || in.trusted == nullptr || ! (in.sampleRate > 0.0))
        return out;

    // Step 4 up front: the prominence test in step 5 needs the smoothed curve,
    // and smoothing is over the WHOLE array -- a bin outside the trusted band
    // still contributes to the average of a bin inside it.
    std::array<float, kNumBins> hs {};
    smooth (in.hDb, hs.data(), kNumBins, in.sampleRate);

    // Top-kMaxPreventivePerLane by H_dB descending, maintained by insertion so
    // step 7 keeps the HOTTEST six, not the first six found.
    std::array<Candidate, kMaxPreventivePerLane> best {};
    int kept = 0;

    for (int k = 0; k < kNumBins; ++k)
    {
        const auto i  = (std::size_t) k;
        const double hz = LoopGainEstimator::binToHz (k, in.sampleRate);

        // Step 1: both sides Hz.
        if (hz < SoundcheckSignal::kSweepLowHz || hz > LoopGainEstimator::kTrustedHighHz)
            continue;

        // Step 2.
        if (! in.trusted[i])
            continue;

        // Step 3: +-1 bin around anything already notched on this lane (inv 15).
        // A notch is wider than one bin, so stacking a second cut on the
        // neighbour of a live one over-cuts the same mode.
        bool nearLiveNotch = false;
        if (in.liveNotchHz != nullptr)
        {
            for (int n = 0; n < in.liveNotchCount; ++n)
            {
                const int lb = LoopGainEstimator::hzToBin ((double) in.liveNotchHz[(std::size_t) n],
                                                           in.sampleRate);
                if (std::abs (k - lb) <= 1)
                {
                    nearLiveNotch = true;
                    break;
                }
            }
        }
        if (nearLiveNotch)
            continue;

        const double h = (double) in.hDb[i];

        // Step 5: hot enough to show the operator, AND a peak rather than a
        // slope. Both sides of both comparisons are dB.
        if (h < kCandidateMarginDb)
            continue;
        if (h - (double) hs[i] < kMinProminenceDb)
            continue;

        out.marked[i] = true;
        ++out.markedCount;

        // Step 6: only a marked bin, and only when the rule returned a cut.
        // depthDb == 0 is the rule's "no proposal" -- it covers both the
        // below-kMinUsefulCutDb case and a preset whose ceiling allows no cut
        // at all, and neither may become a 0 dB notch in a chain slot.
        const Depth d = depthFor (h, in.ceilingDb, in.ladder);
        if (! (d.depthDb < 0.0))
            continue;

        Candidate c;
        c.hz         = (float) hz;
        c.marginDb   = (float) -h;          // the GUI's "margin" column: -H_dB
        c.depthDb    = (float) d.depthDb;
        c.q          = (float) in.notchQ;
        c.residualDb = (float) d.residualDb;
        c.bin        = k;

        // Step 7. marginDb == -H_dB, so hottest-first means marginDb ASCENDING.
        int pos = 0;
        while (pos < kept && best[(std::size_t) pos].marginDb <= c.marginDb)
            ++pos;                          // <= keeps the lower bin first on a tie

        if (pos >= kMaxPreventivePerLane)
            continue;                       // colder than all six already held

        for (int j = std::min (kept, kMaxPreventivePerLane - 1); j > pos; --j)
            best[(std::size_t) j] = best[(std::size_t) (j - 1)];

        best[(std::size_t) pos] = c;
        if (kept < kMaxPreventivePerLane)
            ++kept;
    }

    out.candidates     = best;
    out.candidateCount = kept;

    // Counted over the SURVIVING proposals, so the number the GUI shows and the
    // per-candidate residualDb it shows beside it always describe the same set.
    for (int i = 0; i < kept; ++i)
        if (out.candidates[(std::size_t) i].residualDb > 0.0f)
            ++out.saturatedBins;

    return out;
}
