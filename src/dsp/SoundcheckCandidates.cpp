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

    // An empty or null ladder is a CALLER BUG, and the silent failure it would
    // otherwise cause is the dangerous kind: the rung loop would run zero
    // times, `found` would stay false, and every proposal would land on
    // maxDepthDb -- a full -24 dB cut on every bin, from a struct nobody
    // filled in. Refuse instead, with the rule's own "no proposal" Depth.
    if (ladder.rungsDb == nullptr || ladder.count <= 0)
        return out;

    // The same refusal for a ceiling that is not a number, and for the same
    // reason: std::max is (a < b) ? b : a, so std::max (rung, NaN) compares
    // `rung < NaN` -- false -- and hands back the RAW RUNG. A direct caller
    // would get a fully formed -12 dB proposal with saturated == false out of
    // an Input nobody filled in. pick() raises Output::ceilingMissing for its
    // own callers; depthFor is public and has no such channel, so it refuses.
    if (! std::isfinite (ceilingDb))
        return out;

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

    // Computed BEFORE the null-input return below, so a caller always gets a
    // truthful ceilingMissing. A controller that forgot the ceiling has most
    // likely forgotten the buffers too, and reporting "no proposals, ceiling
    // fine" on the way out of that would point the investigation at the room.
    //
    // Neither of these stops the MARKS: the operator is still shown the room,
    // and a caller bug must not silently look like a quiet stage. Both stop
    // every PROPOSAL, because the depth of a cut cannot be decided without a
    // ladder to quantise onto or a ceiling to clamp against.
    //
    // DELIBERATELY REDUNDANT with depthFor's own guard: while both stand, no
    // test can tell them apart (measured 2026-09-16 -- neutering either one
    // alone leaves the suite green; removing BOTH turns
    // EmptyLadderMarksButProposesNothing red). Keep both anyway. depthFor is
    // public and a later lane may call it directly, and this one states the
    // refusal where the marks/proposals split actually happens.
    const bool ladderMissing  = (in.ladder.rungsDb == nullptr || in.ladder.count <= 0);
    const bool ceilingMissing = ! std::isfinite (in.ceilingDb);
    out.ceilingMissing = ceilingMissing;

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

        // Step 1: both sides Hz. Note the low edge is `hz < kSweepLowHz`, so
        // at 48 kHz bin 4 (93.75 Hz) is EXCLUDED here, while the estimator's
        // band-SNR window starts at hzToBin(kSweepLowHz) == bin 4 and INCLUDES
        // it. The two are different things -- an aggregate SNR figure versus a
        // per-bin proposal gate -- and the one-bin difference has no functional
        // consequence: see AboveTheTrustedBandIsNeverACandidate on why a bin
        // that low can never pass step 5 anyway.
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

        // A NaN passes EVERY `<` comparison below -- `NaN < x` is false, so a
        // NaN bin would survive both step 5 gates untouched and reach depthFor,
        // where `needed < kMinUsefulCutDb` is false too: it would be proposed
        // at maxDepthDb with saturated == false. Reject it explicitly.
        //
        // Also deliberately redundant with the NaN-safe prominence gate below
        // (a NaN in hDb makes prom NaN too, and that gate rejects it). Removing
        // EITHER alone leaves the suite green; removing both turns
        // NonFiniteLoopGainIsNeverMarked red. This one is the explicit,
        // readable statement of the rule and does not depend on how the
        // prominence comparison happens to be spelled.
        if (! std::isfinite (h))
            continue;

        // Step 5: hot enough to show the operator, AND a peak rather than a
        // slope. Both sides of both comparisons are dB.
        if (h < kCandidateMarginDb)
            continue;

        // Written as `! (prom >= k)` and not `prom < k` so that a NaN REJECTS
        // the bin. hs[i] is NaN whenever a non-finite bin fell inside this
        // bin's 1/3-octave window, and with `prom < k` that comparison would be
        // false and the bin would pass the prominence gate it never satisfied.
        const double prom = h - (double) hs[i];
        if (! (prom >= kMinProminenceDb))
            continue;

        out.marked[i] = true;
        ++out.markedCount;

        // Step 6: only a marked bin, and only when the rule returned a cut.
        // depthDb == 0 is the rule's "no proposal" -- it covers both the
        // below-kMinUsefulCutDb case and a preset whose ceiling allows no cut
        // at all, and neither may become a 0 dB notch in a chain slot.
        if (ladderMissing || ceilingMissing)
            continue;

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
