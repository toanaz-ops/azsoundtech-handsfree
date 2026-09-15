// src/dsp/SoundcheckCandidates.h
//
// Lane M, spec §4.4. Turns one LoopGainEstimator::Result into two lists: the
// bins the operator should SEE (marked) and the bins the app offers to CUT
// (candidates), each carrying the exact depth the cut would use.
//
// THIS CLASS DECIDES HOW MUCH GAIN COMES OUT OF A PA. Every number below is a
// dB in the same convention lane G uses everywhere: DEEPER IS MORE NEGATIVE,
// so a clamp against a shallower value is std::max, never std::min.
//
//   needed_dB  = H_dB[k] + kTargetMarginDb   // dB that must be CUT; > 0 = a cut is needed
//   needed_dB < kMinUsefulCutDb  ->  MARK only, no proposal
//   depth_raw  = -needed_dB
//   rung       = SHALLOWEST rung of the ladder at least as deep as depth_raw
//                (the largest r satisfying r <= depth_raw); none -> maxDepthDb
//   depth      = max (rung, ceilingDb)       // a shallower preset ceiling WINS (Q13)
//   depth      = max (depth, maxDepthDb)     // lane G invariant 1
//   residualDb = max (0, needed_dB - (-depth))
//   saturated  = residualDb > 0
//
// WHY MARK AND PROPOSE ARE DIFFERENT THRESHOLDS (F22). kCandidateMarginDb
// (-6 dB) marks; kMinUsefulCutDb (3 dB) proposes. A bin at H_dB = -5.9 needs
// 0.1 dB: the operator should SEE it, but spending the shallowest rung (-6 dB)
// and one of sixteen chain slots on it over-cuts by 5.9 dB. Because a proposal
// needs `needed >= 3`, the -6 rung can never over-cut by more than 3 dB.
//
// WHY `saturated` IS REPORTED AND NOT SWALLOWED. A silent -24 on a bin that
// needed -36 is a false promise. Both ways a proposal can come up short -- the
// ladder floor and the preset ceiling -- set residualDb, so the GUI can say how
// many dB are still over after the deepest cut the app is allowed to make.
//
// WHY THE LADDER IS A PARAMETER AND NOT AN INCLUDE. kDepthLadderDb /
// kDepthLadderSize / kMaxDepthDb are lane G's, declared on NotchController
// (src/app/NotchController.h). Including app/NotchController.h from src/dsp/
// would invert the layering and drag in app/PresetManager.h and juce_events;
// copying the four numbers would create the second-literal problem lane G spent
// m-D removing. So the caller passes them, and the tests pass THE SHIPPED
// LADDER -- the six worked examples are asserted against the app's own array.
//
// THREADING. Pure arithmetic on caller-owned buffers; no state, no allocation
// beyond the returned Output, no device. Lane M's controller thread calls it.
//
// Deliberately in src/dsp/ with no src/app/ include, exactly as
// LoopGainEstimator.h is.
#pragma once

#include "dsp/LoopGainEstimator.h"

#include <array>
#include <limits>

class SoundcheckCandidates
{
public:
    static constexpr int    kNumBins              = LoopGainEstimator::kNumBins;
    static constexpr double kCandidateMarginDb    = -6.0;   // MARK threshold, on H_dB
    static constexpr double kMinUsefulCutDb       =  3.0;   // PROPOSE threshold, on needed_dB
    static constexpr double kMinProminenceDb      =  6.0;   // H_dB above its own 1/3-octave average
    static constexpr double kTargetMarginDb       =  6.0;   // dB of headroom a cut aims to leave
    static constexpr int    kMaxPreventivePerLane =  6;

    // The depth ladder, supplied by the caller. SoundcheckController (an app/
    // class) builds this from NotchController::kDepthLadderDb /
    // kDepthLadderSize / kMaxDepthDb; see the header comment.
    struct Ladder
    {
        const double* rungsDb = nullptr;
        int           count   = 0;
        double        maxDepthDb = -24.0;
    };

    // depthDb == 0.0 means "no proposal" -- the bin is MARK only.
    struct Depth { double depthDb = 0.0; double residualDb = 0.0; bool saturated = false; };

    // The whole depth rule, on one H_dB value. Public so the six worked
    // examples of spec §4.4 can be asserted directly instead of inferred from
    // a full pick() run.
    [[nodiscard]] static Depth depthFor (double hDb, double ceilingDb, const Ladder& ladder);

    struct Input
    {
        const float* hDb        = nullptr;      // kNumBins, from LoopGainEstimator
        // kNumBins. Pass LoopGainEstimator::Result::trusted VERBATIM: pick()
        // has no `measured` input and cannot re-derive that gate, and Result
        // documents that `measured` OVERRIDES `trusted` -- finish() has already
        // cleared the whole array when the run was unusable. A caller that
        // rebuilds this array from per-bin SNR instead reintroduces exactly the
        // hole Task 2's review closed.
        const bool*  trusted    = nullptr;
        double sampleRate       = 0.0;
        // THREE DISTINCT STATES, and the difference decides whether a PA gets
        // cut at all:
        //   NaN  = UNSET. A controller bug, not a preset. pick() computes marks,
        //          proposes NOTHING, and raises Output::ceilingMissing.
        //   0.0  = no cut authorised by this preset. Marks only, silently and
        //          legitimately.
        //   < 0  = the running preset's ceiling, the shallowest cut allowed.
        // SoundcheckController MUST write this from the running preset. The
        // default is NaN rather than 0.0 so a forgotten assignment is
        // DETECTABLE instead of looking like a preset that allows no cut.
        double ceilingDb        = std::numeric_limits<double>::quiet_NaN();
        double notchQ           = 0.0;
        const float* liveNotchHz = nullptr;     // frequencies of notches already live on this lane
        int    liveNotchCount    = 0;
        Ladder ladder {};
    };

    struct Candidate
    {
        float hz = 0.0f, marginDb = 0.0f, depthDb = 0.0f, q = 0.0f, residualDb = 0.0f;
        int   bin = 0;
    };

    struct Output
    {
        std::array<Candidate, kMaxPreventivePerLane> candidates {};
        int candidateCount = 0, markedCount = 0, saturatedBins = 0;
        // Input::ceilingDb was not finite -- see that field. The run produced
        // marks and no proposals, and the reason is a BUG in the caller, not a
        // quiet room: Task 6 must surface it rather than showing an empty
        // proposal list as a clean result.
        bool ceilingMissing = false;
        std::array<bool, kNumBins> marked {};
    };

    // The seven steps of spec §4.4, in order -- reordering them changes results:
    //   1. drop every bin outside [kSweepLowHz, kTrustedHighHz];
    //   2. drop every bin with trusted[k] == false;
    //   3. drop every bin within +-1 bin of a frequency in liveNotchHz (inv 15);
    //   4. smooth hDb with a 1/3-octave moving average;
    //   5. MARK when hDb >= kCandidateMarginDb AND hDb - smoothed >= kMinProminenceDb;
    //   6. PROPOSE only a marked bin whose depthFor() returned a proposal;
    //   7. sort by hDb DESCENDING, keep at most kMaxPreventivePerLane.
    //
    // THREE WAYS pick() REFUSES TO PROPOSE WHILE STILL MARKING. Each is a
    // caller error that must not be allowed to become a wrong cut, and in each
    // the operator still gets to SEE the room:
    //   - Input::ladder has no rungs (rungsDb == nullptr or count <= 0) -- with
    //     no ladder every proposal would silently land on maxDepthDb;
    //   - Input::ceilingDb is not finite -- see that field; Output::ceilingMissing;
    //   - hDb[k] is not finite -- a NaN passes every `<` comparison, so it is
    //     rejected explicitly before step 5 rather than reaching depthFor.
    [[nodiscard]] static Output pick (const Input& in);

    // 1/3-octave moving average of hDb. Public so a later lane can reuse it;
    // it has no test of its own and is exercised through pick(), from BOTH
    // sides (m-19, corrected 2026-09-16 by mutation):
    //
    //   too WIDE / too flat -- a smoother returning the whole-array mean makes
    //     the speaker roll-off look prominent, and SpeakerRolloffIsNotACandidate
    //     goes red;
    //   too NARROW / a no-op -- a smoother returning its input makes every
    //     prominence exactly zero, so NOTHING is ever marked. That does NOT
    //     turn SpeakerRolloffIsNotACandidate red (it asserts zero candidates
    //     and would still get zero); the tests that catch it are the ones that
    //     require a candidate to EXIST -- MarginIsTheNegativeOfLoopGain,
    //     AtMostSixPerLaneAndTheHottestSurvive, UntrustedBinIsNeverACandidate,
    //     SaturatesAtMinusTwentyFourAndReportsResidual,
    //     MarkedButNotProposedBelowMinUsefulCut and the control half of
    //     AboveTheTrustedBandIsNeverACandidate.
    //
    // THE WINDOW IS ONE-SIDED IN THE TOP SIXTH OF AN OCTAVE. `hi` is clamped to
    // numBins - 1, so for a bin above Nyquist / 2^(1/6) the average is taken
    // over a window that extends below the centre and not above it, and the
    // result is biased by the local slope. That is harmless as used here --
    // step 1 has already dropped everything above kTrustedHighHz (6 kHz, far
    // below the clamp at any supported rate) -- but a later lane that calls
    // smooth() for its own purposes must treat the result as meaningful only
    // BELOW kTrustedHighHz.
    static void smooth (const float* hDb, float* out, int numBins, double sampleRate);
};
