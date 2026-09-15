// tests/test_soundcheck_candidates.cpp
//
// The depth rule decides how deep a cut lands on a live PA, so every one of
// spec §4.4's six worked examples is asserted as an EXACT number here. The
// ladder comes from NotchController, not from a copy: a test that quantises
// against its own array proves nothing about the app.
#include <gtest/gtest.h>

#include "app/NotchController.h"
#include "dsp/SoundcheckCandidates.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;

SoundcheckCandidates::Ladder shippedLadder()
{
    return { NotchController::kDepthLadderDb,
             NotchController::kDepthLadderSize,
             NotchController::kMaxDepthDb };
}

struct Field
{
    std::vector<float> hDb  = std::vector<float> (SoundcheckCandidates::kNumBins, -40.0f);
    std::vector<char>  trust = std::vector<char> (SoundcheckCandidates::kNumBins, 1);

    void poke (double hz, float value)
    {
        hDb[(std::size_t) LoopGainEstimator::hzToBin (hz, kSr)] = value;
    }

    SoundcheckCandidates::Input input (double ceilingDb = -24.0) const
    {
        SoundcheckCandidates::Input in;
        in.hDb        = hDb.data();
        in.trusted    = reinterpret_cast<const bool*> (trust.data());
        in.sampleRate = kSr;
        in.ceilingDb  = ceilingDb;
        in.notchQ     = 30.0;
        in.ladder     = shippedLadder();
        return in;
    }
};
} // namespace

// RED IF: the sign of `needed` is flipped back to rev 1's -(H_dB + 6). That
// produces a POSITIVE depth, and setNotchImpl refuses depth > 0 outright
// (NotchController.cpp:214) -- so the whole feature would place nothing, in
// silence. F6.
TEST (SoundcheckCandidates, DepthSignIsNegative)
{
    const auto d = SoundcheckCandidates::depthFor (2.0, -24.0, shippedLadder());

    EXPECT_DOUBLE_EQ (d.depthDb, -12.0);
    EXPECT_DOUBLE_EQ (d.residualDb, 0.0);
    EXPECT_FALSE (d.saturated);

    for (double h = -6.0; h <= 30.0; h += 0.25)
    {
        const auto x = SoundcheckCandidates::depthFor (h, -24.0, shippedLadder());
        if (x.depthDb != 0.0)
        {
            ASSERT_LT (x.depthDb, 0.0)  << "H_dB=" << h;
            ASSERT_GE (x.depthDb, NotchController::kMaxDepthDb) << "H_dB=" << h;
        }
    }
}

// RED IF: the quantiser is stated backwards ("the DEEPEST rung not deeper than
// depth_raw"), which rev 2 of the spec did: that gives -6 for a -8 requirement
// and an EMPTY set for -5. N2.
TEST (SoundcheckCandidates, DepthQuantisesOntoTheLadder)
{
    EXPECT_DOUBLE_EQ (SoundcheckCandidates::depthFor (-1.0, -24.0, shippedLadder()).depthDb,  -6.0);
    EXPECT_DOUBLE_EQ (SoundcheckCandidates::depthFor ( 2.0, -24.0, shippedLadder()).depthDb, -12.0);
    EXPECT_DOUBLE_EQ (SoundcheckCandidates::depthFor ( 8.0, -24.0, shippedLadder()).depthDb, -18.0);
    EXPECT_DOUBLE_EQ (SoundcheckCandidates::depthFor (14.0, -24.0, shippedLadder()).depthDb, -24.0);
}

// RED IF: the ceiling is applied with min() instead of max(), or applied before
// the quantisation. Deeper is MORE NEGATIVE, so a shallower ceiling wins with
// std::max -- the same convention lane G uses at every clamp.
TEST (SoundcheckCandidates, CeilingClampsTheProposal)
{
    const auto d = SoundcheckCandidates::depthFor (2.0, -10.0, shippedLadder());
    EXPECT_DOUBLE_EQ (d.depthDb, -10.0);
    EXPECT_DOUBLE_EQ (d.residualDb, 0.0);
}

// RED IF: someone quantises the CEILING onto a rung. presets/Music.json ships
// -10.0 (read 2026-09-15); quantising it to -6 makes Music 4 dB shallower than
// 1.1.3 with nobody reporting it -- this is lane G's Q13 defect, and lane M
// must not reintroduce it. inv 14, second half.
TEST (SoundcheckCandidates, CeilingNotMultipleOfSixEndsOnTheCeiling)
{
    const auto d = SoundcheckCandidates::depthFor (2.0, -10.0, shippedLadder());
    EXPECT_DOUBLE_EQ (d.depthDb, -10.0);
    EXPECT_NE (d.depthDb, -6.0);
    EXPECT_NE (d.depthDb, -12.0);
}

// RED IF: saturation is swallowed. A -24 on a bin that needed -36 is a promise
// the app cannot keep, and the operator has to be told.
TEST (SoundcheckCandidates, SaturatesAtMinusTwentyFourAndReportsResidual)
{
    const auto d = SoundcheckCandidates::depthFor (30.0, -24.0, shippedLadder());

    EXPECT_DOUBLE_EQ (d.depthDb, -24.0);
    EXPECT_NEAR (d.residualDb, 12.0, 1.0e-9);
    EXPECT_TRUE (d.saturated);

    Field f;
    f.poke (1000.0, 30.0f);
    const auto out = SoundcheckCandidates::pick (f.input (-24.0));
    EXPECT_EQ (out.saturatedBins, 1);
}

// RED IF: the ceiling path forgets to report residual. This is the SECOND way a
// proposal can come up short, and spec §4.4 requires both to be named.
TEST (SoundcheckCandidates, SaturationByTheCeilingIsAlsoReported)
{
    const auto d = SoundcheckCandidates::depthFor (14.0, -10.0, shippedLadder());

    EXPECT_DOUBLE_EQ (d.depthDb, -10.0);
    EXPECT_NEAR (d.residualDb, 10.0, 1.0e-9);
    EXPECT_TRUE (d.saturated);
}

// RED IF: kMinUsefulCutDb is dropped and kCandidateMarginDb is used for both
// jobs. A bin needing 0.1 dB would then eat a -6 dB notch and one of sixteen
// chain slots. F22.
TEST (SoundcheckCandidates, MarkedButNotProposedBelowMinUsefulCut)
{
    Field f;
    f.poke (1000.0, -5.9f);

    const auto out = SoundcheckCandidates::pick (f.input());

    EXPECT_EQ (out.markedCount, 1);
    EXPECT_EQ (out.candidateCount, 0);
    EXPECT_TRUE (out.marked[(std::size_t) LoopGainEstimator::hzToBin (1000.0, kSr)]);
}

// RED IF: prominence is dropped. A loudspeaker's own high-frequency roll-off is
// a smooth slope with no modes in it and must produce NOTHING.
TEST (SoundcheckCandidates, SpeakerRolloffIsNotACandidate)
{
    Field f;
    for (int k = 0; k < SoundcheckCandidates::kNumBins; ++k)
    {
        const double hz = LoopGainEstimator::binToHz (k, kSr);
        f.hDb[(std::size_t) k] = (float) (6.0 - 12.0 * std::log10 (std::max (hz, 20.0) / 100.0));
    }

    const auto out = SoundcheckCandidates::pick (f.input());
    EXPECT_EQ (out.candidateCount, 0);
}

// RED IF: the +-1 bin exclusion is dropped or narrowed to exact equality.
// Notching a bin that already has a live notch stacks two cuts on one mode.
// inv 15.
TEST (SoundcheckCandidates, BinWithALiveNotchIsSkipped)
{
    Field f;
    f.poke (1000.0, 10.0f);

    auto in = f.input();
    // One bin BELOW the hot bin -- the +-1 window must still exclude it.
    const float liveHz = (float) LoopGainEstimator::binToHz (
        LoopGainEstimator::hzToBin (1000.0, kSr) - 1, kSr);
    in.liveNotchHz    = &liveHz;
    in.liveNotchCount = 1;

    const auto out = SoundcheckCandidates::pick (in);
    EXPECT_EQ (out.candidateCount, 0);
}

// RED IF: kMaxPreventivePerLane stops being enforced, or the survivors are the
// first six found rather than the six hottest.
//
// B-7: mind the sign. The list is sorted by H_dB DESCENDING (hottest first), and
// marginDb == -H_dB, so along the sorted list marginDb ASCENDS -- it gets more
// negative. Plan rev 1 asserted it the other way round and would have failed on
// a correct implementation.
TEST (SoundcheckCandidates, AtMostSixPerLaneAndTheHottestSurvive)
{
    Field f;
    const double hz[]  = { 200.0, 400.0, 630.0, 1000.0, 1600.0, 2500.0, 4000.0, 5000.0 };
    const float  hot[] = {  1.0f, 20.0f,  3.0f,  18.0f,   5.0f,  16.0f,   7.0f,  14.0f };
    for (int i = 0; i < 8; ++i)
        f.poke (hz[i], hot[i]);

    const auto out = SoundcheckCandidates::pick (f.input());

    EXPECT_EQ (out.candidateCount, SoundcheckCandidates::kMaxPreventivePerLane);
    EXPECT_LE (out.candidates[0].marginDb, out.candidates[1].marginDb)
        << "hottest first means MOST NEGATIVE margin first";

    // The two coldest (H_dB 1.0 and 3.0, i.e. margin -1 and -3) must be dropped,
    // so every survivor has margin <= -5.
    for (int i = 0; i < out.candidateCount; ++i)
        ASSERT_LE (out.candidates[i].marginDb, -5.0f);
}

// RED IF: an untrusted bin can still become a candidate.
//
// m-17: the bin must sit INSIDE [kSweepLowHz, kTrustedHighHz], or step 1 of pick
// drops it on the band test alone and the trusted[] flag is never consulted --
// plan rev 1 poked 8 kHz, which proved nothing. 2 kHz is inside the band, so the
// only thing that can reject it is trusted == false.
TEST (SoundcheckCandidates, UntrustedBinIsNeverACandidate)
{
    Field f;
    f.poke (2000.0, 25.0f);

    // Control: trusted, it IS a candidate.
    ASSERT_EQ (SoundcheckCandidates::pick (f.input()).candidateCount, 1);

    f.trust[(std::size_t) LoopGainEstimator::hzToBin (2000.0, kSr)] = 0;
    EXPECT_EQ (SoundcheckCandidates::pick (f.input()).candidateCount, 0);
}

// RED IF: marginDb is published as H_dB instead of -H_dB. The GUI labels this
// column "margin", and a sign error there reads as "this room is fine".
TEST (SoundcheckCandidates, MarginIsTheNegativeOfLoopGain)
{
    Field f;
    f.poke (1000.0, 9.0f);

    const auto out = SoundcheckCandidates::pick (f.input());
    ASSERT_EQ (out.candidateCount, 1);
    EXPECT_NEAR (out.candidates[0].marginDb, -9.0f, 1.0e-4f);
    EXPECT_NEAR (out.candidates[0].hz, 1000.0f, (float) (kSr / Detector::kFftSize));
    EXPECT_DOUBLE_EQ (out.candidates[0].depthDb, -18.0);
}

// RED IF: step 1 of the pick order -- drop every bin outside
// [kSweepLowHz, kTrustedHighHz] -- is dropped.
//
// NOT IN THE TASK BRIEF, added 2026-09-16 after mutation testing: removing the
// band test entirely left all twelve of the brief's tests GREEN. In production
// LoopGainEstimator already clears trusted[] above kTrustedHighHz, so step 2
// hides the gap -- but pick() is a pure function that must not lean on its
// caller's invariant, and this fixture is the only one that sets trusted == true
// on a bin the sweep never trusted.
//
// The LOW half of the band test (below kSweepLowHz) is not asserted because it
// is unreachable: at 48 kHz a bin below 100 Hz sits at index <= 4, and a
// +-2^(1/6) window around it rounds to that single bin, so its prominence is
// exactly 0 and step 5 rejects it before step 1 could matter.
TEST (SoundcheckCandidates, AboveTheTrustedBandIsNeverACandidate)
{
    Field f;
    f.poke (8000.0, 25.0f);   // trusted[] left true on purpose -- see above

    const auto out = SoundcheckCandidates::pick (f.input());

    EXPECT_EQ (out.candidateCount, 0);
    EXPECT_EQ (out.markedCount, 0);

    // Control: the identical poke INSIDE the band is a candidate, so the zero
    // above is the band test and not a broken fixture.
    Field g;
    g.poke (2000.0, 25.0f);
    EXPECT_EQ (SoundcheckCandidates::pick (g.input()).candidateCount, 1);
}
