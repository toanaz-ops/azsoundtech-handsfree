// tests/test_notchcontroller.cpp
#include <gtest/gtest.h>
#include "app/NotchController.h"
#include "dsp/PeakinessAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace {
class FakeClock : public ClockSource {
public:
    double nowMs() const override { return ms_; }
    void advance (double m) { ms_ += m; }
private:
    double ms_ = 1000.0;
};

struct Harness {
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    FakeClock clock;
    NotchController controller { tap, commands, clock };
};

// Same shape as Harness but with an explicit routing slot id.
struct SlotHarness {
    int slotId;
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    FakeClock clock;
    NotchController controller { tap, commands, clock, slotId };

    explicit SlotHarness (int s) : slotId (s) {}
};

// Two rings, one controller: the lane-S shape MainComponent builds.
struct StereoHarness {
    LockFreeRingBuffer<float> tapL { 8192 };
    LockFreeRingBuffer<float> tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    FakeClock clock;
    NotchController controller { tapL, &tapR, commands, clock };
};

std::vector<PresetNotch> onePresetNotch (int index, double freq)
{
    PresetNotch n;
    n.index   = index;
    n.freq    = freq;
    n.Q       = 30.0;
    n.depthDB = -12.0;
    return { n };
}

std::vector<PresetNotch> twoPresetNotches()
{
    PresetNotch a;
    a.index = 0; a.freq = 1000.0; a.Q = 30.0; a.depthDB = -12.0;
    PresetNotch b;
    b.index = 1; b.freq = 1250.0; b.Q = 30.0; b.depthDB = -12.0;
    return { a, b };
}
}

TEST (NotchControllerValidation, RejectsBadParamsWithoutTouchingModelOrQueue)
{
    Harness h;
    EXPECT_FALSE (h.controller.setNotch (0, 0, 24000.0, 30.0, -12.0, NotchController::Origin::Detector)); // >= Nyquist of 48k
    EXPECT_FALSE (h.controller.setNotch (0, 0, 1000.0,  0.0, -12.0, NotchController::Origin::Detector));  // Q <= 0
    EXPECT_FALSE (h.controller.setNotch (0, 0, 1000.0, 30.0,  +3.0, NotchController::Origin::Detector));  // depth > 0
    EXPECT_FALSE (h.controller.setNotch (2, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));  // channel OOR
    EXPECT_FALSE (h.controller.setNotch (0, 16, 1000.0, 30.0, -12.0, NotchController::Origin::Detector)); // index OOR
    EXPECT_EQ (h.commands.getAvailableRead(), 0u);
}

TEST (NotchControllerCommands, SetNotchRecordsModelAndEnqueues)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (1, 3, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    h.controller.runOnce();  // flush outbox
    ASSERT_EQ (h.commands.getAvailableRead(), 1u);
    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.type, NotchCommandType::Set);
    EXPECT_EQ (cmd.channel, 1);
    EXPECT_EQ (cmd.index, 3);
    EXPECT_FLOAT_EQ (cmd.frequency, 1000.0f);
    EXPECT_FLOAT_EQ (cmd.depthDB, -12.0f);
}

TEST (NotchControllerCommands, ClearEmitsClearCommandOnlyWhenActive)
{
    Harness h;
    h.controller.clearNotch (0, 5);   // nothing active -> nothing queued
    EXPECT_EQ (h.commands.getAvailableRead(), 0u);
    ASSERT_TRUE (h.controller.setNotch (0, 5, 800.0, 30.0, -12.0, NotchController::Origin::Detector));
    h.controller.clearNotch (0, 5);
    h.controller.runOnce();
    ASSERT_EQ (h.commands.getAvailableRead(), 2u);
    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.type, NotchCommandType::Set);
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.type, NotchCommandType::Clear);
    EXPECT_EQ (cmd.index, 5);
}

TEST (NotchControllerCommands, FullQueueDelaysButNeverLoses)
{
    Harness h;
    for (int i = 0; i < 140; ++i)  // 140 > capacity 128
        ASSERT_TRUE (h.controller.setNotch (0, i % 16, 500.0 + i, 30.0, -12.0, NotchController::Origin::Detector));
    h.controller.runOnce();
    EXPECT_GT (h.controller.retryCount(), 0u);
    NotchCommand batch[160];
    const auto first  = h.commands.read (batch, 160);
    h.controller.runOnce();          // retry the unsent tail
    const auto second = h.commands.read (batch + first, 160);
    EXPECT_EQ (first + second, 140u);  // nothing lost
}

TEST (NotchControllerAutoRelease, NotchSurvivesDeadTapAfter30s)   // D-06
{
    Harness h;  // tap never fed => dead forever
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    h.clock.advance (31000.0);
    h.controller.runOnce();

    NotchCommand cmd {};
    std::size_t sets = 0, clears = 0;
    while (h.commands.read (&cmd, 1) == 1)
        (cmd.type == NotchCommandType::Clear ? clears : sets)++;
    EXPECT_EQ (sets, 1u);
    EXPECT_EQ (clears, 0u);
}

TEST (NotchControllerLiveClock, AlternatingEmptyPollsTrackWallTime)  // half-speed bug guard
{
    Harness h;
    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 400; ++i) {
        if (i % 2 == 0)                       // every OTHER poll delivers data
            h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    EXPECT_NEAR (h.controller.liveMsForTest(), 2000.0, 60.0);
}

// Lane G: the 1.1.3 cliff is now a ladder. The claim is unchanged -- an
// un-reinforced notch eventually leaves -- only the clock moved. From -12:
// -6 at 30 s, Clear at 40 s (task-7 brief's rung table).
TEST (NotchControllerAutoRelease, LiveTapReleasesThroughTheLadderAfter40s)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 8800; ++i) {          // 8800 * 5 ms = 44 s of live audio
        h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    h.controller.runOnce();                   // make sure everything is flushed

    NotchCommand cmd {};
    bool sawClearOfSlot0 = false;
    while (h.commands.read (&cmd, 1) == 1)
        if (cmd.type == NotchCommandType::Clear && cmd.channel == 0 && cmd.index == 0)
            sawClearOfSlot0 = true;
    EXPECT_TRUE (sawClearOfSlot0);
}

TEST (NotchControllerSnapshot, PublishesSpectrumWithBinsAndRate)
{
    Harness h;
    std::vector<float> hop (512, 0.25f);
    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_EQ (snap.magnitudeCount, (std::uint32_t) Detector::kNumBins);
    EXPECT_DOUBLE_EQ (snap.sampleRate, 48000.0);
    EXPECT_GE (snap.sequence, 1u);
}

TEST (NotchControllerSnapshot, NotchAndSpectrumShareOneInstant)
{
    Harness h;
    std::vector<float> hop (512, 0.25f);

    // Frame 1: no notches yet.
    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();
    NotchController::SnapshotBuffer first;
    h.controller.copySnapshot (first);
    EXPECT_EQ (first.notchCount, 0u);

    // Frame 2: a notch added afterwards appears WITH the newer spectrum,
    // never painted over the older one -- same sequence bump, same frame.
    ASSERT_TRUE (h.controller.setNotch (0, 4, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();
    NotchController::SnapshotBuffer second;
    h.controller.copySnapshot (second);

    EXPECT_GT (second.sequence, first.sequence);
    ASSERT_EQ (second.notchCount, 1u);
    EXPECT_EQ (second.notches[0].channel, 0);
    EXPECT_EQ (second.notches[0].index, 4);
    EXPECT_FLOAT_EQ (second.notches[0].frequency, 1000.0f);
}

TEST (NotchControllerSnapshot, CopyIsValueSemanticsNoAliasing)
{
    Harness h;
    std::vector<float> hop (512, 0.25f);
    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();

    NotchController::SnapshotBuffer a;
    h.controller.copySnapshot (a);
    const auto seqA = a.sequence;

    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();

    // 'a' must be untouched by later publications.
    EXPECT_EQ (a.sequence, seqA);
}

TEST (NotchControllerPreset, AdoptsWithPresetOriginOnBothChannels)
{
    Harness h;
    std::vector<PresetNotch> notches (6);
    for (int i = 0; i < 6; ++i) {
        notches[i].index   = i;
        notches[i].freq    = 500.0 + 100.0 * i;
        notches[i].Q       = 30.0;
        notches[i].depthDB = -12.0;
    }

    EXPECT_EQ (h.controller.adoptPreset (notches), 6);
    h.controller.runOnce();

    // 6 notches x 2 channels = 12 Set commands (design §2 burst sizing).
    EXPECT_EQ (h.commands.getAvailableRead(), 12u);

    NotchController::SnapshotBuffer snap;
    h.tap.write (std::vector<float> (512, 0.25f).data(), 512);
    h.controller.runOnce();
    h.controller.copySnapshot (snap);
    ASSERT_EQ (snap.notchCount, 12u);
}

TEST (NotchControllerPreset, InvalidNotchesAreSkippedNotHalfApplied)
{
    Harness h;
    std::vector<PresetNotch> notches (2);
    notches[0].index = 0; notches[0].freq = 1000.0;  notches[0].Q = 30.0; notches[0].depthDB = -12.0;
    notches[1].index = 1; notches[1].freq = 24000.0; notches[1].Q = 30.0; notches[1].depthDB = -12.0;  // >= Nyquist of 48k

    EXPECT_EQ (h.controller.adoptPreset (notches), 1);
    h.controller.runOnce();
    EXPECT_EQ (h.commands.getAvailableRead(), 2u);   // only the valid one, both channels
}

TEST (NotchControllerPreset, AdoptedPresetsAutoReleaseLikeDetectorNotches)   // D-05
{
    Harness h;
    std::vector<PresetNotch> notches (1);
    notches[0].index = 0; notches[0].freq = 1000.0; notches[0].Q = 30.0; notches[0].depthDB = -12.0;
    ASSERT_EQ (h.controller.adoptPreset (notches), 1);

    std::vector<float> hop (512, 0.1f);
    // Preset notches ride the lane-G release ladder like any other non-
    // Soundcheck notch (only KD-7 exempts anything). From -12: -6 at 30 s,
    // Clear at 40 s. 8800 * 5 ms = 44 s.
    for (int i = 0; i < 8800; ++i) {
        h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    h.controller.runOnce();

    bool sawClearOfSlot0BothChannels = false;
    bool clearCh0 = false, clearCh1 = false;
    NotchCommand cmd {};
    while (h.commands.read (&cmd, 1) == 1)
        if (cmd.type == NotchCommandType::Clear && cmd.index == 0) {
            if (cmd.channel == 0) clearCh0 = true;
            if (cmd.channel == 1) clearCh1 = true;
        }
    sawClearOfSlot0BothChannels = clearCh0 && clearCh1;
    EXPECT_TRUE (sawClearOfSlot0BothChannels);
}

// ===========================================================================
// Detection policy loop (spec 5.2 steps 6-7, plan KD-5..KD-9).
//
// These tests push REAL sine tones through Harness.tap so the Detector's FFT
// produces genuine candidates (same proven pattern as tests/test_peakiness.cpp:
// a local PeakinessAnalyzer sanity-checks that candidates exist before the
// policy outcomes are asserted). Simulated time advances 512/48000 s = 10.667 ms
// per block, one hop per pump, so the clock looks natural to every timer.
// ===========================================================================
namespace
{
constexpr double kTestPi    = 3.14159265358979323846;
constexpr double kTestSr    = 48000.0;
constexpr double kBlockMs   = Detector::kHopSize / kTestSr * 1000.0;   // 10.667 ms
constexpr int    kWarmupBlocks = 64;   // ~683 ms of scorer history (>= 450 ms rise reference)

struct SineSource
{
    double freq       = 1000.0;   // bin 43 = 1007.8125 Hz at 48 kHz (2048 FFT)
    float  amp        = 1.0f;     // loud howl, ~26 dB over the quiet floor
    double nextSample = 0.0;

    std::vector<float> hop()
    {
        std::vector<float> out ((std::size_t) Detector::kHopSize);
        for (int i = 0; i < Detector::kHopSize; ++i)
        {
            out[(std::size_t) i] = amp * static_cast<float> (
                std::sin (2.0 * kTestPi * freq * nextSample / kTestSr));
            nextSample += 1.0;
        }
        return out;
    }
};

struct NoiseSource
{
    float amp = 0.01f;
    std::mt19937 rng { 777u };   // fixed seed: a flaky test is worse than no test
    std::uniform_real_distribution<float> dist { -1.0f, 1.0f };

    std::vector<float> hop()
    {
        std::vector<float> out ((std::size_t) Detector::kHopSize);
        for (int i = 0; i < Detector::kHopSize; ++i)
            out[(std::size_t) i] = amp * dist (rng);
        return out;
    }
};

void pump (Harness& h, const std::vector<float>& hop)
{
    ASSERT_EQ (h.tap.write (hop.data(), hop.size()), hop.size());
    h.clock.advance (kBlockMs);
    h.controller.runOnce();
}

// Pumps ambient noise for `ms` of LIVE time. The tap stays alive (D-06) so
// the release clock runs, and the noise never feeds peakiness at 1 kHz.
void pumpQuietFor (Harness& h, NoiseSource& quiet, double ms)
{
    const int blocks = (int) std::lround (ms / kBlockMs);
    for (int i = 0; i < blocks; ++i)
        pump (h, quiet.hop());
}

// A findable helper for "which index is holding a notch on this lane" -- the
// room-memory tests all need it and none of them may use the depth to answer
// it (B-3: pushClearLocked lowers `active` and LEAVES depthDB behind, so a
// depth probe matches every slot that has ever held a notch).
int firstActiveIndex (NotchController& c, int lane)
{
    for (int k = 0; k < NotchController::kSlots; ++k)
        if (c.activeForTest (lane, k))
            return k;
    return -1;
}

// A room-memory PROBE: asks "what depth does a fresh howl at `freq` get?" and
// leaves the controller as it found it. Detection must already be active and
// the scorer warm.
//
// It plays a hard-onset tone until something is placed, reads that depth, and
// takes the notch away with a MANUAL clear -- manual, because only an
// AutoRelease writes a memory entry and a probe must not leave one behind.
//
// The clear runs on EVERY lane, not just the one probed. A mono Harness has
// taps_[1] == nullptr, so effectiveLinked() is true and a placement writes the
// pair; clearing lane 0 alone leaves lane 1's notch standing, and those
// leftovers accumulate in the harmonic-penalty's `locked` list
// (processSpectrumForDetection, KD-3), where they quietly stop later probes at
// other bins from confirming at all. Measured: a probe sequence 40, 41, 42, 60
// placed three times and then went dead.
// The quiet lead-in is load-bearing when several probes run back to back: it
// flushes the previous probe's tone out of the 2048-sample analysis window and
// out of the scorer's 117 ms rise reference, so what confirms next is THIS
// tone and not a leftover candidate at the last one's bin.
//
// Returns 0.0 when nothing was ever placed -- no caller's expected depth,
// so the caller's own assertion reports it.
double probeMemoryAt (Harness& h, NoiseSource& quiet, double freq)
{
    for (int i = 0; i < 24; ++i)
        pump (h, quiet.hop());

    SineSource tone; tone.freq = freq;
    for (int i = 0; i < 80; ++i)
    {
        pump (h, tone.hop());
        const int placed = firstActiveIndex (h.controller, 0);
        if (placed >= 0)
        {
            const double depth = h.controller.depthDbForTest (0, placed);
            for (int lane = 0; lane < NotchController::kChannels; ++lane)
                h.controller.clearNotch (lane, placed, NotchController::ClearReason::Manual);
            return depth;
        }
    }
    return 0.0;
}

// A tone that CREEPS up out of the noise instead of switching on. SineSource
// starts at full amplitude in one block, so its rise ratio is enormous and
// every test built on it places at -12; the -6 start of spec 4.3 is only
// observable behind a slow build (M-12).
//
// WHY IT STARTS INAUDIBLE (B-5, and this is the load-bearing part): the
// scorer's rise reference is the 11th frame back, 117.3 ms old. If the tone
// switched on ABOVE the noise floor, that reference would be a NOISE frame for
// the first 11 blocks and riseRatio would be the tone-to-noise ratio -- 64x at
// amp 0.02 -- regardless of the slope. And peakiness is scale-invariant
// (PeakinessAnalyzer.cpp:59-76), so a pure tone confirms as soon as the 2048
// window is all tone (~4 blocks) -- i.e. INSIDE that window. Starting BELOW
// the noise floor is the only way to build a rise history that is tone-vs-
// tone: it holds rNorm and mNorm under their thresholds until the reference
// frame is itself tone, which is block 15 (ref = the first all-tone window at
// block 4). Measured: this fixture confirms at tone block 16.
//
// kRampStartAmp, DERIVED then MEASURED. The derivation: NoiseSource (uniform
// +-0.01, sigma 5.77e-3) has a per-bin magnitude of sigma * sqrt(sum w^2) =
// 0.16 in a Hann 2048 window; a sine of amplitude A lands at A * sum(w)/2 =
// A * 512, so they are equal at A = 3.1e-4. Running it put 3.0e-4 at
// riseRatio 2.12 -- STILL the steep branch -- because the tone phase feeds no
// noise UNDER the ramp: peakiness is scale-invariant and saturates (~105) as
// soon as the window is all tone, so the confirm is gated only by rNorm and
// mNorm and it landed at block ~11, where the 117.3 ms reference is still the
// LAST NOISE FRAME. Starting at the floor is not enough; the tone must start
// BELOW it, so that no axis can cross before the reference is itself tone.
//
// Measured plateau (sweep at rev 3, this machine, Release):
//   3.0e-4 -> 2.117 (FAILS: noise reference)   2.8e-4 -> 1.967 (marginal)
//   2.5e-4 -> 1.744    2.2e-4 -> 1.7507    1.5e-4 -> 1.7507    1.0e-5 -> 1.7507
// From 1e-5 to 2.2e-4 the ratio is flat at 1.7507 with refAgeMs 117.33 -- the
// signature of a tone-vs-TONE comparison, where the ratio is a property of the
// SLOPE alone and not of the start level. It is NOT the 11-hop figure the
// timestamp implies (r^11 = 1.04778^11 = 1.6708, the derivation's 1.671; a
// geometric ramp's window weighting cancels exactly, so the 4-hop FFT window
// does not widen it): the ratio is actually r^12 = 1.04778^12 = 1.75065 --
// TWELVE hops of ramp gain measured against an ELEVEN-hop timestamp
// (refAgeMs 117.33 = 11 x 10.667 ms) -- a one-hop offset between the
// history's age label and the window content it is compared against. 1.5e-4
// sits mid-plateau: 2x below the value that fails, and a decade above where
// the score margin over kConfirmScore thins.
//
// +9.5 dB / 250 ms: over the 117.3 ms gap that is riseRatio ~1.75 -- above the
// 1.5 that saturates rNorm, below kSteepRiseRatio 2.0.
constexpr float kRampStartAmp = 1.5e-4f;   // BELOW the noise floor (0.16 bin mag)

struct RampSineSource
{
    double freq        = 1000.0;
    float  amp         = kRampStartAmp;   // starts BELOW the noise floor, never over it
    double gainDbPerMs = 9.5 / 250.0;
    double nextSample  = 0.0;

    std::vector<float> hop()
    {
        std::vector<float> out ((std::size_t) Detector::kHopSize);
        for (int i = 0; i < Detector::kHopSize; ++i)
        {
            out[(std::size_t) i] = amp * static_cast<float> (
                std::sin (2.0 * kTestPi * freq * nextSample / kTestSr));
            nextSample += 1.0;
        }
        // 0.9 clamp: at 38 dB/s this source passes full scale at ~2 s, and a
        // clipped tone is a different signal with a different spectrum. The
        // caller stops long before here; this is the second line of defence.
        amp = static_cast<float> (std::min (0.9, static_cast<double> (amp)
                                  * std::pow (10.0, gainDbPerMs * kBlockMs / 20.0)));
        return out;
    }
};

// primeAndPlace's shape, driven by a source that ramps. Returns the index of
// the first Set, or -1. Stops at 150 tone blocks = 1.6 s = +60.8 dB, where the
// amplitude is ~0.16 -- comfortably under the 0.9 clamp, and 134 blocks past
// the 16 where the confirm measures. Pumping further would only reach the
// clamp and then confirm on novelty alone from a FLAT tone, which would
// quietly be testing something else. A -1 here means kRampStartAmp needs
// retuning, not more blocks.
int primeAndPlaceSlowly (Harness& h)
{
    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    RampSineSource tone;
    for (int i = 0; i < 150; ++i)
    {
        pump (h, tone.hop());
        NotchCommand cmd {};
        if (h.commands.read (&cmd, 1) == 1 && cmd.type == NotchCommandType::Set)
            return cmd.index;
    }
    return -1;
}

void pumpStereo (StereoHarness& h, const std::vector<float>& left, const std::vector<float>& right)
{
    ASSERT_EQ (h.tapL.write (left.data(),  left.size()),  left.size());
    ASSERT_EQ (h.tapR.write (right.data(), right.size()), right.size());
    h.clock.advance (kBlockMs);
    h.controller.runOnce();
}

// True when SOME bin currently looks like a candidate in the published
// snapshot -- the same peakiness test analyse() applies before its
// local-maximum rule.
bool anyCandidateInSnapshot (Harness& h)
{
    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    if (snap.magnitudeCount == 0)
        return false;
    for (int bin = PeakinessAnalyzer::kNeighbourOuterRadius;
         bin < Detector::kNumBins - PeakinessAnalyzer::kNeighbourOuterRadius;
         ++bin)
    {
        if (PeakinessAnalyzer::peakinessAt (snap.magnitudes[0].data(),
                                            Detector::kNumBins, bin)
                > PeakinessAnalyzer::kDefaultThreshold)
        {
            return true;
        }
    }
    return false;
}

// Warm the scorer with ambient noise (detection already enabled), then feed
// tone blocks until a Set pair lands. Returns the slot index via `slot`.
void primeAndPlace (Harness& h, int& slot)
{
    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    SineSource tone;
    slot = -1;
    for (int i = 0; i < 40 && slot < 0; ++i)
    {
        pump (h, tone.hop());
        if (h.commands.getAvailableRead() >= 2)
        {
            NotchCommand cmd {};
            ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
            EXPECT_EQ (cmd.type, NotchCommandType::Set);
            EXPECT_EQ (cmd.channel, 0);
            slot = cmd.index;
        }
    }
}
} // namespace

TEST (NotchControllerDetection, DetectionInactivePlacesNothing)
{
    Harness h;
    SineSource tone;   // loud steady tone, detection never enabled
    for (int i = 0; i < 120; ++i)
        pump (h, tone.hop());
    EXPECT_EQ (h.commands.getAvailableRead(), 0u);

    // Positive control of the rig itself: candidates WERE there to be had.
    EXPECT_TRUE (anyCandidateInSnapshot (h));
}

TEST (NotchControllerDetection, PersistentHowlSetsNotchOnBothChannels)
{
    Harness h;
    h.controller.setDetectionActive (true);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);

    // Exactly ONE slot filled per channel: ch0 Set (read in primeAndPlace),
    // then exactly one more command -- the ch1 Set at the SAME index.
    ASSERT_EQ (h.commands.getAvailableRead(), 1u);
    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.type, NotchCommandType::Set);
    EXPECT_EQ (cmd.channel, 1);
    EXPECT_EQ (cmd.index, slot);
    // Runtime defaults (brief 2026-08-24): Q 30. Depth is now the LADDER's
    // (lane G, spec 4.3), not notchDepthDb_: SineSource switches its tone on
    // hard, so riseRatio is far past kSteepRiseRatio and the notch starts on
    // the second rung. -18 arrives later, via the deepen path (Task 6).
    EXPECT_FLOAT_EQ (cmd.Q, 30.0f);
    EXPECT_FLOAT_EQ (cmd.depthDB, -12.0f);
    // Frequency within half a bin (11.72 Hz) of the 1 kHz tone.
    EXPECT_NEAR (cmd.frequency, 1000.0, 0.5 * kTestSr / Detector::kFftSize);
}

TEST (NotchControllerDetection, TwoBlocksOnlyDoesNotSet)
{
    Harness h;
    h.controller.setDetectionActive (true);

    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    // Feed tone only until the FIRST confirming-quality candidate appears,
    // plus exactly one more block => at most TWO consecutive confirms, one
    // short of kPersistenceBlocks.
    SineSource tone;
    bool sawCandidate = false;
    for (int i = 0; i < 20 && ! sawCandidate; ++i)
    {
        pump (h, tone.hop());
        sawCandidate = anyCandidateInSnapshot (h);
    }
    ASSERT_TRUE (sawCandidate);   // teeth: there WAS something to confirm
    pump (h, tone.hop());

    // Silence (ambient floor) for plenty of blocks: nothing may ever fire.
    for (int i = 0; i < 250; ++i)
        pump (h, quiet.hop());
    EXPECT_EQ (h.commands.getAvailableRead(), 0u);
}

TEST (NotchControllerDetection, HowlThatStopsAutoReleasesThroughTheLadder)
{
    Harness h;
    h.controller.setDetectionActive (true);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);

    // Drain the remaining ch1 Set so later assertions see only new traffic.
    NotchCommand cmd {};
    while (h.commands.read (&cmd, 1) == 1)
        ;

    // Howl stops: low-level ambient noise keeps the tap alive (D-06) but never
    // feeds peakiness at the notch bin, so the release ladder runs to the end.
    // The notch was detector-placed and deepened against the -18 default
    // ceiling, so the Clear is at 30 + 10 + 10 = 50 s (task-7 rung table).
    NoiseSource quiet;
    for (int i = 0; i < 5200; ++i)   // 5200 * 10.667 ms ~= 55.5 s
        pump (h, quiet.hop());
    h.controller.runOnce();

    bool c0 = false, c1 = false;
    while (h.commands.read (&cmd, 1) == 1)
        if (cmd.type == NotchCommandType::Clear && cmd.index == slot)
        {
            if (cmd.channel == 0) c0 = true;
            if (cmd.channel == 1) c1 = true;
        }
    EXPECT_TRUE (c0);
    EXPECT_TRUE (c1);
}

TEST (NotchControllerDetection, SoundcheckNotchNeverAutoReleases)
{
    Harness h;
    EXPECT_DOUBLE_EQ (h.controller.getSoundcheckRemainingMs(), 0.0);
    h.controller.startSoundcheck();
    EXPECT_TRUE (h.controller.soundcheckActive());

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);

    // Drain the placement burst so later assertions see only new traffic.
    NotchCommand cmd {};
    while (h.commands.read (&cmd, 1) == 1)
        ;

    // 60 s of live audio: far past both the 15 s soundcheck window and the
    // 30 s auto-release horizon.
    NoiseSource quiet;
    for (int i = 0; i < 5625; ++i)   // 5625 * 10.667 ms ~= 60 s
        pump (h, quiet.hop());
    h.controller.runOnce();

    while (h.commands.read (&cmd, 1) == 1)
        EXPECT_FALSE (cmd.type == NotchCommandType::Clear && cmd.index == slot)
            << "soundcheck notch was auto-released";

    EXPECT_FALSE (h.controller.soundcheckActive());
    EXPECT_DOUBLE_EQ (h.controller.getSoundcheckRemainingMs(), 0.0);
}

TEST (NotchControllerDetection, SoundcheckAutoDisarmsAtExpiryExactlyOnce)
{
    // Spec §5.3: startSoundcheck() arms detection FOR THE DURATION. Before the
    // auto-disarm fix nothing ever cleared detectionActive_, so one press of
    // Soundcheck left the detector live forever.
    Harness h;
    h.controller.startSoundcheck();
    EXPECT_TRUE (h.controller.detectionActiveForTest());

    NoiseSource quiet;
    // ~10 s of live audio: still inside the 15 s window.
    for (int i = 0; i < 900; ++i)   // 900 * 10.667 ms ~= 9.6 s
        pump (h, quiet.hop());
    EXPECT_TRUE (h.controller.soundcheckActive());
    EXPECT_TRUE (h.controller.detectionActiveForTest());

    // Past expiry the controller disarms ITSELF, in the same live time the
    // countdown reports.
    for (int i = 0; i < 700; ++i)   // cumulative ~= 17 s
        pump (h, quiet.hop());
    EXPECT_DOUBLE_EQ (h.controller.getSoundcheckRemainingMs(), 0.0);
    EXPECT_FALSE (h.controller.detectionActiveForTest());

    // Exactly once: later polls see no deadline -- no resurrect, no re-store.
    for (int i = 0; i < 100; ++i)   // cumulative ~= 18 s
        pump (h, quiet.hop());
    EXPECT_FALSE (h.controller.detectionActiveForTest());
    EXPECT_DOUBLE_EQ (h.controller.getSoundcheckRemainingMs(), 0.0);
}

// ===========================================================================
// Multi-slot routing (task 4): every command carries its controller's slot
// id, and only width_ lanes are driven.
// ===========================================================================

TEST (NotchControllerSlotAware, Width1SendsOneLaneTaggedWithSlotId)
{
    SlotHarness h (3);
    h.controller.setWidth (1);

    EXPECT_EQ (h.controller.adoptPreset (twoPresetNotches()), 2);
    h.controller.runOnce();

    // 2 notches x 1 lane = 2 commands (NOT 4).
    ASSERT_EQ (h.commands.getAvailableRead(), 2u);
    NotchCommand cmd {};
    for (int i = 0; i < 2; ++i)
    {
        ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
        EXPECT_EQ (cmd.type, NotchCommandType::Set);
        EXPECT_EQ (cmd.slot, 3);
        EXPECT_EQ (cmd.channel, 0);   // lane 1 must never appear at width 1
    }
}

TEST (NotchControllerSlotAware, SetWidthClampsIntoRange)
{
    {
        SlotHarness h (0);
        h.controller.setWidth (5);   // clamps to 2 lanes
        EXPECT_EQ (h.controller.adoptPreset (onePresetNotch (0, 1000.0)), 1);
        h.controller.runOnce();
        EXPECT_EQ (h.commands.getAvailableRead(), 2u);
    }
    {
        SlotHarness h (0);
        h.controller.setWidth (0);   // clamps up to 1 lane
        EXPECT_EQ (h.controller.adoptPreset (onePresetNotch (0, 1000.0)), 1);
        h.controller.runOnce();
        EXPECT_EQ (h.commands.getAvailableRead(), 1u);
    }
}

TEST (NotchControllerSlotAware, PolicyApiRejectsLanesBeyondWidth)
{
    SlotHarness h (0);
    h.controller.setWidth (1);

    // Lane 1 is outside width: setNotch must reject, clearNotch must no-op --
    // neither may ever leave a command whose channel exceeds width.
    EXPECT_FALSE (h.controller.setNotch (1, 0, 1000.0, 30.0, -12.0,
                                         NotchController::Origin::Detector));
    h.controller.clearNotch (1, 0);
    h.controller.runOnce();
    EXPECT_EQ (h.commands.getAvailableRead(), 0u);

    // Lane 0 still works normally at width 1.
    EXPECT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Detector));
    h.controller.runOnce();
    EXPECT_EQ (h.commands.getAvailableRead(), 1u);
}

TEST (NotchControllerSlotAware, DefaultsMatchLegacyBehaviour)
{
    Harness h;   // slotId 0, default width 2 -- exactly the old controller
    EXPECT_EQ (h.controller.adoptPreset (onePresetNotch (0, 1000.0)), 1);
    h.controller.runOnce();

    ASSERT_EQ (h.commands.getAvailableRead(), 2u);
    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.slot, 0);
    EXPECT_EQ (cmd.channel, 0);
    EXPECT_EQ (cmd.index, 0);
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.slot, 0);
    EXPECT_EQ (cmd.channel, 1);
    EXPECT_EQ (cmd.index, 0);
}

// ===========================================================================
// Two-lane analysis bundles (stereo-aware detection, Task 2). Placement
// policy is UNCHANGED here -- these only assert the snapshot and tuning
// surface now carry both lanes.
// ===========================================================================

// Lane S task 2. Red if the snapshot stops carrying lane 1's spectrum or
// misreports laneCount.
TEST (NotchControllerStereo, SnapshotPublishesBothLanesSpectraAndLaneCount)
{
    StereoHarness h;
    SineSource toneL;                 // 1 kHz on the left only
    NoiseSource quietR;                // default amp: brief's own harness
    for (int i = 0; i < 8; ++i)
        pumpStereo (h, toneL.hop(), quietR.hop());

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_EQ (snap.laneCount, 2u);
    EXPECT_FALSE (snap.linked);
    ASSERT_EQ (snap.magnitudeCount, (std::uint32_t) Detector::kNumBins);

    const int toneBin = 43;   // 1007.8 Hz @ 48 kHz / 2048
    // Pins the lane-1 copy itself (not just the ratio): if the lane-1 branch
    // of runOnce()'s copy loop were deleted, magnitudes[1] would stay all-
    // zero and this would fail even though the ratio check below still
    // trivially "passes" (anything > 0).
    EXPECT_GT (snap.magnitudes[1][toneBin], 0.0f);
    EXPECT_GT (snap.magnitudes[0][toneBin], 10.0f * snap.magnitudes[1][toneBin]);
}

// Red if the legacy ctor reports a lane-1 tap it does not have.
TEST (NotchControllerStereo, LegacyCtorHasNoLaneOneTap)
{
    Harness h;
    EXPECT_FALSE (h.controller.hasLaneOneTapForTest());
    StereoHarness s;
    EXPECT_TRUE (s.controller.hasLaneOneTapForTest());
}

// Red if a tuning setter reaches only lane 0's analyser.
TEST (NotchControllerStereo, TuningSettersReachBothLanes)
{
    StereoHarness h;
    h.controller.setPeakinessThreshold (15.0f);
    h.controller.setRiseReferenceMs (500.0);
    EXPECT_FLOAT_EQ (h.controller.getPeakinessThreshold (0), 15.0f);
    EXPECT_FLOAT_EQ (h.controller.getPeakinessThreshold (1), 15.0f);
    EXPECT_DOUBLE_EQ (h.controller.getRiseReferenceMs (1), 500.0);
}

// Red if runOnce()'s drain loop (NotchController.cpp, the `for (;;)` /
// `if (! any) break;` loop around the per-lane processLatestBlock() calls)
// were collapsed to a single pass instead of draining every block a large
// audio callback delivered at once -- e.g. replacing the `for (;;)` with a
// plain `if`. Detector::processLatestBlock() reads at most one hop
// (kHopSize samples) per call, so writing 3 hops into BOTH rings before a
// single runOnce() must still publish 3 snapshots, advancing `sequence` by
// exactly 3.
TEST (NotchControllerStereo, RunOnceDrainsMultipleBlocksPerCall)
{
    StereoHarness h;
    SineSource toneL;
    NoiseSource quietR;
    std::vector<float> left, right;
    for (int i = 0; i < 3; ++i)
    {
        const auto l = toneL.hop();
        const auto r = quietR.hop();
        left.insert  (left.end(),  l.begin(), l.end());
        right.insert (right.end(), r.begin(), r.end());
    }
    ASSERT_EQ (h.tapL.write (left.data(),  left.size()),  left.size());
    ASSERT_EQ (h.tapR.write (right.data(), right.size()), right.size());
    h.clock.advance (kBlockMs);

    NotchController::SnapshotBuffer before;
    h.controller.copySnapshot (before);
    h.controller.runOnce();
    NotchController::SnapshotBuffer after;
    h.controller.copySnapshot (after);

    EXPECT_EQ (after.sequence - before.sequence, 3u);
}

// Same drain-loop assertion, legacy mono-ring shape (Harness, one tap).
TEST (NotchControllerStereo, RunOnceDrainsMultipleBlocksPerCallLegacy)
{
    Harness h;
    SineSource tone;
    std::vector<float> samples;
    for (int i = 0; i < 3; ++i)
    {
        const auto hop = tone.hop();
        samples.insert (samples.end(), hop.begin(), hop.end());
    }
    ASSERT_EQ (h.tap.write (samples.data(), samples.size()), samples.size());
    h.clock.advance (kBlockMs);

    NotchController::SnapshotBuffer before;
    h.controller.copySnapshot (before);
    h.controller.runOnce();
    NotchController::SnapshotBuffer after;
    h.controller.copySnapshot (after);

    EXPECT_EQ (after.sequence - before.sequence, 3u);
}

// ===========================================================================
// Detection tuning (brief 2026-08-24): runtime rise reference, persistence
// blocks, notch defaults and peakiness threshold.
// ===========================================================================

TEST (NotchControllerTuning, RiseReferenceClamps)
{
    Harness h;
    h.controller.setRiseReferenceMs (50.0);
    EXPECT_DOUBLE_EQ (h.controller.getRiseReferenceMs(), 100.0);
    h.controller.setRiseReferenceMs (2000.0);
    EXPECT_DOUBLE_EQ (h.controller.getRiseReferenceMs(), 1000.0);
    h.controller.setRiseReferenceMs (400.0);
    EXPECT_DOUBLE_EQ (h.controller.getRiseReferenceMs(), 400.0);
}

TEST (NotchControllerTuning, PersistenceBlocksClamps)
{
    Harness h;
    h.controller.setPersistenceBlocks (0);
    EXPECT_EQ (h.controller.getPersistenceBlocks(), 1);
    h.controller.setPersistenceBlocks (99);
    EXPECT_EQ (h.controller.getPersistenceBlocks(), 10);
    h.controller.setPersistenceBlocks (4);
    EXPECT_EQ (h.controller.getPersistenceBlocks(), 4);
}

TEST (NotchControllerTuning, NotchDefaultsClamp)
{
    Harness h;
    h.controller.setNotchDefaults (5.0, -2.0);   // under both floors
    EXPECT_DOUBLE_EQ (h.controller.getNotchQ(), 8.0);
    EXPECT_DOUBLE_EQ (h.controller.getNotchDepthDb(), -6.0);
    h.controller.setNotchDefaults (80.0, -30.0); // over both ceilings
    EXPECT_DOUBLE_EQ (h.controller.getNotchQ(), 50.0);
    EXPECT_DOUBLE_EQ (h.controller.getNotchDepthDb(), -24.0);
}

TEST (NotchControllerTuning, PeakinessThresholdForwardsAndClamps)
{
    Harness h;
    h.controller.setPeakinessThreshold (15.0f);
    EXPECT_FLOAT_EQ (h.controller.getPeakinessThreshold(), 15.0f);
    h.controller.setPeakinessThreshold (1.0f);
    EXPECT_FLOAT_EQ (h.controller.getPeakinessThreshold(), 5.0f);
    h.controller.setPeakinessThreshold (99.0f);
    EXPECT_FLOAT_EQ (h.controller.getPeakinessThreshold(), 20.0f);
}

TEST (NotchControllerDetection, LowerRiseReferenceConfirmsFromYoungHistory)
{
    // Mirror of CandidateScorer::YoungHistoryDoesNotClaimARise: with the old
    // 500 ms / default 250 ms reference, ~64 ms of history is too young to
    // claim a rise from, so no notch can appear within a few tone blocks.
    // With the reference dropped to 100 ms the SAME history qualifies.
    auto placeWithin = [] (int howlBudget)
    {
        Harness h;
        h.controller.setDetectionActive (true);
        // Persistence 1: this test isolates the RISE reference; the
        // persistence knob has its own test below.
        h.controller.setPersistenceBlocks (1);

        NoiseSource quiet;
        for (int i = 0; i < 6; ++i)          // ~64 ms of scorer history
            pump (h, quiet.hop());

        SineSource tone;
        for (int i = 0; i < howlBudget; ++i)
        {
            pump (h, tone.hop());
            if (h.commands.getAvailableRead() >= 2)
                return true;
        }
        return false;
    };

    EXPECT_TRUE (placeWithin (8));
}

TEST (NotchControllerDetection, DefaultRiseReferenceStillNeedsDeepHistory)
{
    // The control half: identical rig, DEFAULT reference -- the same budget
    // must NOT place, because every history frame is younger than the
    // minimum age (0.45 * 250 ms = 112.5 ms).
    Harness h;
    h.controller.setDetectionActive (true);

    NoiseSource quiet;
    for (int i = 0; i < 6; ++i)
        pump (h, quiet.hop());

    SineSource tone;
    for (int i = 0; i < 4; ++i)
        pump (h, tone.hop());
    EXPECT_EQ (h.commands.getAvailableRead(), 0u);
}

TEST (NotchControllerDetection, PersistenceBlocksOnePlacesAfterSingleConfirm)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setPersistenceBlocks (1);

    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    SineSource tone;
    bool placed = false;
    for (int i = 0; i < 20 && ! placed; ++i)
    {
        pump (h, tone.hop());
        placed = h.commands.getAvailableRead() > 0;
    }
    ASSERT_TRUE (placed);

    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_EQ (cmd.type, NotchCommandType::Set);
}

TEST (NotchControllerDetection, SetNotchDefaultsFlowIntoPlacedNotch)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (20.0, -24.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);

    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
    EXPECT_FLOAT_EQ (cmd.Q, 20.0f);
    // Lane G: the depth default is now the CEILING (Q1), not the starting
    // depth. A ceiling of -24 permits the whole ladder, and SineSource's hard
    // start puts the first rung at -12.
    EXPECT_FLOAT_EQ (cmd.depthDB, -12.0f);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -12.0);
}

TEST (NotchControllerDetection, DetectionCommandsCarrySlotIdOnBothLanes)
{
    LockFreeRingBuffer<float> tap { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    FakeClock clock;
    NotchController controller { tap, commands, clock, 5 };
    controller.setDetectionActive (true);

    auto pumpOne = [&] (const std::vector<float>& hop) {
        ASSERT_EQ (tap.write (hop.data(), hop.size()), hop.size());
        clock.advance (kBlockMs);
        controller.runOnce();
    };

    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpOne (quiet.hop());

    SineSource tone;
    for (int i = 0; i < 40 && commands.getAvailableRead() < 2; ++i)
        pumpOne (tone.hop());

    ASSERT_GE (commands.getAvailableRead(), 2u);
    NotchCommand cmd {};
    bool sawLane0 = false, sawLane1 = false;
    while (commands.read (&cmd, 1) == 1)
    {
        ASSERT_EQ (cmd.type, NotchCommandType::Set);
        EXPECT_EQ (cmd.slot, 5) << "detection command not tagged with slot id";
        if (cmd.channel == 0) sawLane0 = true;
        if (cmd.channel == 1) sawLane1 = true;
    }
    EXPECT_TRUE (sawLane0);
    EXPECT_TRUE (sawLane1);
}

// ===========================================================================
// Stereo placement policy (task 3): INDEP by default, LINK on request,
// lane-aware index search (S-7), asymmetry bonus (design §4.3, §4.4).
// ===========================================================================
namespace {
// Drains every command currently queued into `out`.
std::vector<NotchCommand> drain (LockFreeRingBuffer<NotchCommand>& ring)
{
    std::vector<NotchCommand> out;
    NotchCommand cmd {};
    while (ring.read (&cmd, 1) == 1)
        out.push_back (cmd);
    return out;
}

// Warm both lanes with noise, then feed `left`/`right` sources until at
// least one Set appears or 40 blocks pass. Returns everything queued.
template <typename L, typename R>
std::vector<NotchCommand> warmThenDrive (StereoHarness& h, L& left, R& right)
{
    NoiseSource quietL, quietR;
    quietR.rng.seed (999u);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());
    for (int i = 0; i < 40; ++i)
    {
        pumpStereo (h, left.hop(), right.hop());
        if (h.commands.getAvailableRead() > 0)
            break;
    }
    // A few more blocks so a lagging second lane (if any) gets its chance.
    for (int i = 0; i < 6; ++i)
        pumpStereo (h, left.hop(), right.hop());
    return drain (h.commands);
}
} // namespace

// Spec test 10. Red if narrowing to mono orphans lane-1 notches in the model.
TEST (NotchControllerSlotAware, NarrowingToMonoClearsLaneOneNotches)
{
    StereoHarness h;
    ASSERT_TRUE (h.controller.setNotch (1, 2, 800.0, 30.0, -12.0, NotchController::Origin::Manual));
    ASSERT_TRUE (h.controller.setNotch (0, 2, 800.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.runOnce();
    (void) drain (h.commands);

    h.controller.setWidth (1);       // thread is not running in tests: precondition holds
    h.controller.runOnce();          // flushes the outbox
    const auto cmds = drain (h.commands);
    ASSERT_EQ (cmds.size(), 1u);
    EXPECT_EQ (cmds[0].type, NotchCommandType::Clear);
    EXPECT_EQ (cmds[0].channel, 1);
    EXPECT_EQ (cmds[0].index, 2);

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    for (std::uint32_t i = 0; i < snap.notchCount; ++i)
        EXPECT_EQ (snap.notches[i].channel, 0);
}

// Spec test 4. Red if INDEP placement fans out to the silent lane.
TEST (NotchControllerStereo, IndepHowlOnRightOnlyCutsRightOnly)
{
    StereoHarness h;
    h.controller.setDetectionActive (true);
    NoiseSource quietL;  SineSource toneR;
    const auto cmds = warmThenDrive (h, quietL, toneR);

    ASSERT_FALSE (cmds.empty());
    for (const auto& c : cmds)
    {
        EXPECT_EQ (c.type, NotchCommandType::Set);
        EXPECT_EQ (c.channel, 1) << "lane 0 must stay untouched";
    }
    EXPECT_NEAR (cmds.front().frequency, 1000.0, 0.5 * kTestSr / Detector::kFftSize);
}

// Spec test 5. Red if a both-lane howl yields fewer than one Set per lane, AND
// red if a lane's confirm fans out to the other lane. "A Set on each channel"
// alone proves nothing about independence -- 1.0.4 fanned every confirm to both
// channels and would have passed that. So the two lanes howl at DIFFERENT
// frequencies (1 kHz left, 3 kHz right) and every Set must carry the frequency
// of the lane it landed on; a fan-out would put 1 kHz on channel 1.
TEST (NotchControllerStereo, IndepHowlOnBothLanesCutsBoth)
{
    StereoHarness h;
    h.controller.setDetectionActive (true);
    SineSource toneL;  toneL.freq = 1000.0;   // bin 43 -> 1007.8125 Hz
    SineSource toneR;  toneR.freq = 3000.0;   // bin 128 -> 3000.0 Hz exactly
    const auto cmds = warmThenDrive (h, toneL, toneR);

    constexpr double halfBinHz = 0.5 * kTestSr / Detector::kFftSize;
    bool sawL = false, sawR = false;
    for (const auto& c : cmds)
    {
        ASSERT_EQ (c.type, NotchCommandType::Set);
        if (c.channel == 0)
        {
            sawL = true;
            EXPECT_NEAR (c.frequency, 1000.0, halfBinHz) << "lane 0 notched lane 1's howl";
        }
        else
        {
            sawR = true;
            EXPECT_NEAR (c.frequency, 3000.0, halfBinHz) << "lane 1 notched lane 0's howl";
        }
    }
    EXPECT_TRUE (sawL);
    EXPECT_TRUE (sawR);
}

// Spec test 6. Red if LINKED stops fanning a right-only howl to both lanes
// at the same index.
TEST (NotchControllerStereo, LinkedHowlOnRightOnlyCutsBothAtOneIndex)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setDetectionActive (true);
    NoiseSource quietL;  SineSource toneR;
    const auto cmds = warmThenDrive (h, quietL, toneR);

    ASSERT_GE (cmds.size(), 2u);
    EXPECT_EQ (cmds[0].channel, 0);
    EXPECT_EQ (cmds[1].channel, 1);
    EXPECT_EQ (cmds[0].index, cmds[1].index);
    EXPECT_FLOAT_EQ (cmds[0].frequency, cmds[1].frequency);
}

// Spec test 7 / S-6. Red if the legacy ctor honours INDEP.
TEST (NotchControllerStereo, LegacyCtorIsLinkedRegardlessOfFlag)
{
    Harness h;
    h.controller.setLinked (false);
    EXPECT_TRUE (h.controller.effectiveLinked());
    StereoHarness s;
    EXPECT_FALSE (s.controller.effectiveLinked());
    s.controller.setLinked (true);
    EXPECT_TRUE (s.controller.effectiveLinked());
}

// Spec test 17b / S-7. Red if LINKED reuses an index lane 1 already holds.
TEST (NotchControllerStereo, LinkedPlacementSkipsAnIndexBusyOnEitherLane)
{
    StereoHarness h;
    // Pre-occupy lane 1 index 0 (as INDEP would have) via the policy API.
    ASSERT_TRUE (h.controller.setNotch (1, 0, 3000.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.runOnce();
    (void) drain (h.commands);

    h.controller.setLinked (true);
    h.controller.setDetectionActive (true);
    SineSource toneL; NoiseSource quietR;
    const auto cmds = warmThenDrive (h, toneL, quietR);

    ASSERT_GE (cmds.size(), 2u);
    for (const auto& c : cmds)
    {
        EXPECT_EQ (c.type, NotchCommandType::Set);
        EXPECT_NE (c.index, 0) << "index 0 is busy on lane 1";
    }
}

// Spec test 8. Red if INDEP auto-release on lane 0 waits for lane 1 to go quiet.
TEST (NotchControllerStereo, IndepAutoReleaseIsPerLane)
{
    StereoHarness h;
    h.controller.setDetectionActive (true);
    SineSource toneL, toneR;
    (void) warmThenDrive (h, toneL, toneR);

    // Detection stays ARMED for the whole test. Reinforcement of a still-
    // ringing notch sits behind the detectionActive_ gate, so disarming to
    // "freeze placement" would freeze reinforcement too and release both
    // lanes whatever the policy -- which would measure the gate, not the
    // per-lane rule. The armed detector may keep emitting Sets on the lane
    // that still rings; only Clear commands are asserted on below.
    //
    // Left goes quiet, right keeps ringing, for > 30 s of live time.
    NoiseSource quietL;
    // From the -18 ceiling: 30 + 10 + 10 = 50 s to Clear. Fix round 1, Minor
    // 1: +20 was thin -- integer truncation of each term costs almost a full
    // block, and 4707 (this line, before the fix) left only 18 blocks over
    // the 4689 actually needed:
    //   ceil(30000 / kBlockMs) + ceil(10000 / kBlockMs) + ceil(10000 / kBlockMs)
    //   = 2813 + 938 + 938 = 4689
    // +60 restores a real margin without depending on how the division
    // truncates for the rung this notch stands on.
    const int blocks = (int) ((NotchController::kReleaseFirstMs
                              + 2 * NotchController::kReleaseStepMs) / kBlockMs) + 60;
    for (int i = 0; i < blocks; ++i)
        pumpStereo (h, quietL.hop(), toneR.hop());

    const auto cmds = drain (h.commands);
    bool clearedL = false, clearedR = false;
    for (const auto& c : cmds)
        if (c.type == NotchCommandType::Clear) { clearedL |= (c.channel == 0); clearedR |= (c.channel == 1); }
    EXPECT_TRUE (clearedL);
    EXPECT_FALSE (clearedR);
}

// Spec test 9. Red if a LINKED pair releases while one lane still rings.
TEST (NotchControllerStereo, LinkedAutoReleaseWaitsForBothLanes)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setDetectionActive (true);
    SineSource toneL; NoiseSource quietR;
    (void) warmThenDrive (h, toneL, quietR);

    // Detection stays ARMED for the same reason as IndepAutoReleaseIsPerLane:
    // reinforcement lives behind the gate. Sets the armed detector keeps
    // emitting are ignored; the claim under test is that NO Clear appears.
    //
    // Left goes quiet, RIGHT now rings the same frequency: the pair stays.
    NoiseSource quietL; SineSource toneR;
    // From the -18 ceiling: 30 + 10 + 10 = 50 s to Clear. Fix round 1, Minor
    // 1: +20 was thin -- integer truncation of each term costs almost a full
    // block, and 4707 (this line, before the fix) left only 18 blocks over
    // the 4689 actually needed:
    //   ceil(30000 / kBlockMs) + ceil(10000 / kBlockMs) + ceil(10000 / kBlockMs)
    //   = 2813 + 938 + 938 = 4689
    // +60 restores a real margin without depending on how the division
    // truncates for the rung this notch stands on.
    const int blocks = (int) ((NotchController::kReleaseFirstMs
                              + 2 * NotchController::kReleaseStepMs) / kBlockMs) + 60;
    for (int i = 0; i < blocks; ++i)
        pumpStereo (h, quietL.hop(), toneR.hop());

    for (const auto& c : drain (h.commands))
        EXPECT_NE (c.type, NotchCommandType::Clear);
}

// Spec test 12. Red if the bonus multiplies when the other lane is NOT quiet,
// or fails to multiply when it is. Uses the public score hook below.
TEST (NotchControllerStereo, AsymmetryBonusAppliesOnlyWhenOtherLaneIsQuiet)
{
    StereoHarness h;
    EXPECT_FLOAT_EQ (h.controller.getLaneAsymmetryBonus(), 1.0f);
    h.controller.setLaneAsymmetryBonus (2.0f);
    EXPECT_FLOAT_EQ (h.controller.getLaneAsymmetryBonus(), 2.0f);
    h.controller.setLaneAsymmetryBonus (9.0f);
    EXPECT_FLOAT_EQ (h.controller.getLaneAsymmetryBonus(), 2.0f);   // clamped

    std::array<float, Detector::kNumBins> mine {}, other {};
    mine.fill (1.0f); other.fill (1.0f);
    mine[43] = 100.0f;                 // peaky here
    other[43] = 100.0f;                // other lane equally peaky -> no bonus
    EXPECT_FLOAT_EQ (NotchController::asymmetryMultiplierForTest (mine.data(), other.data(), 43, 2.0f), 1.0f);
    other[43] = 1.0f;                  // other lane flat -> bonus
    EXPECT_FLOAT_EQ (NotchController::asymmetryMultiplierForTest (mine.data(), other.data(), 43, 2.0f), 2.0f);
    EXPECT_FLOAT_EQ (NotchController::asymmetryMultiplierForTest (mine.data(), nullptr, 43, 2.0f), 1.0f);
    EXPECT_FLOAT_EQ (NotchController::asymmetryMultiplierForTest (mine.data(), other.data(), 43, 1.0f), 1.0f);
}

// Spec test 14. Red if INDEP's harmonic penalty sees the other lane's notches.
TEST (NotchControllerStereo, IndepHarmonicPenaltyIsPerLane)
{
    StereoHarness h;
    // Lock 500 Hz on lane 1 only; a 1 kHz howl on lane 0 must NOT be penalised
    // (1 kHz is 2x 500 Hz, inside the 1.4x..4.1x band).
    ASSERT_TRUE (h.controller.setNotch (1, 0, 500.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.runOnce();
    (void) drain (h.commands);
    h.controller.setDetectionActive (true);
    SineSource toneL; NoiseSource quietR;
    const auto cmds = warmThenDrive (h, toneL, quietR);
    bool setOnL = false;
    for (const auto& c : cmds) setOnL |= (c.type == NotchCommandType::Set && c.channel == 0);
    EXPECT_TRUE (setOnL) << "the lane-1 notch must not halve lane 0's score";
}

// Review round 1, Finding 1. Red if placeConfirmed stops resetting the other
// lane's persistence under LINKED: both lanes then confirm in the same drain
// iteration and one howl costs two index pairs (double the cut).
TEST (NotchControllerStereo, LinkedHowlOnBothLanesPlacesExactlyOnePairFirst)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setDetectionActive (true);

    NoiseSource quietL, quietR;
    quietR.rng.seed (999u);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());

    // Identical tone on both lanes: their persistence counters climb in
    // lockstep, so both cross the confirm threshold in the SAME drain
    // iteration -- exactly the race Finding 1 describes.
    SineSource toneL, toneR;
    for (int i = 0; i < 40; ++i)
    {
        pumpStereo (h, toneL.hop(), toneR.hop());
        if (h.commands.getAvailableRead() > 0)
            break;
    }
    const auto cmds = drain (h.commands);

    ASSERT_EQ (cmds.size(), 2u) << "one howl on both lanes must cost exactly one index pair";
    EXPECT_EQ (cmds[0].type, NotchCommandType::Set);
    EXPECT_EQ (cmds[1].type, NotchCommandType::Set);
    EXPECT_EQ (cmds[0].channel, 0);
    EXPECT_EQ (cmds[1].channel, 1);
    EXPECT_EQ (cmds[0].index, cmds[1].index);
    EXPECT_FLOAT_EQ (cmds[0].frequency, cmds[1].frequency);
}

// Review round 2, Finding 1. The same symmetric howl at persistenceBlocks == 1
// -- operator-selectable from the DETECTION panel and what AGGRESSIVE ships.
// Red before the drain-iteration stamp: resetting the other lane's counter is
// a no-op at a required streak of 1 (its next ++ takes it 0 -> 1 >= 1 and it
// confirms in the SAME drain iteration), so one howl bought two index pairs.
TEST (NotchControllerStereo, LinkedHowlOnBothLanesPlacesExactlyOnePairFirstAtPersistOne)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setPersistenceBlocks (1);
    h.controller.setDetectionActive (true);

    NoiseSource quietL, quietR;
    quietR.rng.seed (999u);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());

    SineSource toneL, toneR;
    for (int i = 0; i < 40; ++i)
    {
        pumpStereo (h, toneL.hop(), toneR.hop());
        if (h.commands.getAvailableRead() > 0)
            break;
    }
    const auto cmds = drain (h.commands);

    ASSERT_EQ (cmds.size(), 2u) << "one howl on both lanes must cost exactly one index pair";
    EXPECT_EQ (cmds[0].type, NotchCommandType::Set);
    EXPECT_EQ (cmds[1].type, NotchCommandType::Set);
    EXPECT_EQ (cmds[0].channel, 0);
    EXPECT_EQ (cmds[1].channel, 1);
    EXPECT_EQ (cmds[0].index, cmds[1].index);
    EXPECT_FLOAT_EQ (cmds[0].frequency, cmds[1].frequency);
}

// Spec test 16 (adopt half) / S-8. Red if adoptPreset ignores lane, searches
// for a free index, or lets linked_ change how a file is adopted.
TEST (NotchControllerStereo, AdoptPresetHonoursLaneAndKeepsTheFilesIndex)
{
    StereoHarness h;
    h.controller.setLinked (false);
    PresetNotch both;  both.index = 5; both.freq = 1000.0; both.Q = 30.0; both.depthDB = -12.0;   // lane -1
    PresetNotch right; right.index = 7; right.freq = 2000.0; right.Q = 30.0; right.depthDB = -12.0; right.lane = 1;
    EXPECT_EQ (h.controller.adoptPreset (std::vector<PresetNotch> { both, right }), 2);
    h.controller.runOnce();
    const auto cmds = drain (h.commands);
    ASSERT_EQ (cmds.size(), 3u);
    EXPECT_EQ (cmds[0].channel, 0); EXPECT_EQ (cmds[0].index, 5);
    EXPECT_EQ (cmds[1].channel, 1); EXPECT_EQ (cmds[1].index, 5);
    EXPECT_EQ (cmds[2].channel, 1); EXPECT_EQ (cmds[2].index, 7);
}

// ===========================================================================
// Lane D (data loop): ClearReason on every clear path, NotchEvent sink.
// ===========================================================================
namespace {
using Ev = NotchController::NotchEvent;

// M-5: DECLARE A Recorder BEFORE THE HARNESS IT IS WIRED TO. The controller's
// destructor runs stop(), which flushes its remaining events through the sink
// -- into this object. Declaration order is the reverse of destruction order,
// so a Recorder declared after its Harness is already gone by then and the
// flush writes into a destroyed vector.
struct Recorder
{
    std::vector<Ev> events;
    NotchController::EventSink sink()
    {
        return [this] (const Ev& e) { events.push_back (e); };
    }
    std::vector<Ev> clears() const
    {
        std::vector<Ev> out;
        for (const auto& e : events)
            if (e.kind == Ev::Kind::Clear)
                out.push_back (e);
        return out;
    }
};
} // namespace

// Spec test 8, six reasons. Red if any clear path stops carrying its reason.
TEST (NotchControllerEvents, EveryClearPathCarriesItsReason)
{
    // Manual (default), VerdictFalse, ClearAll -- one Harness.
    {
        Recorder r; Harness h; h.controller.setEventSink (r.sink());
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
        ASSERT_TRUE (h.controller.setNotch (0, 1, 2000.0, 30.0, -12.0, NotchController::Origin::Manual));
        ASSERT_TRUE (h.controller.setNotch (0, 2, 3000.0, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.clearNotch (0, 0);
        h.controller.clearNotch (0, 1, NotchController::ClearReason::VerdictFalse);
        h.controller.clearAll();
        h.controller.runOnce();
        const auto c = r.clears();
        ASSERT_EQ (c.size(), 3u);
        EXPECT_EQ (c[0].reason, NotchController::ClearReason::Manual);       EXPECT_EQ (c[0].index, 0);
        EXPECT_EQ (c[1].reason, NotchController::ClearReason::VerdictFalse); EXPECT_EQ (c[1].index, 1);
        EXPECT_EQ (c[2].reason, NotchController::ClearReason::ClearAll);     EXPECT_EQ (c[2].index, 2);
        EXPECT_FLOAT_EQ (c[2].hz, 3000.0f);
        // Set events preceded them, one per setNotch, no score (manual origin).
        int sets = 0;
        for (const auto& e : r.events) if (e.kind == Ev::Kind::Set) { ++sets; EXPECT_FALSE (e.hasScore); EXPECT_EQ (e.origin, NotchController::Origin::Manual); }
        EXPECT_EQ (sets, 3);
    }
    // AutoRelease: mirror LiveTapReleasesThroughTheLadderAfter40s (above).
    {
        Recorder r; Harness h; h.controller.setEventSink (r.sink());
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
        NoiseSource quiet;
        // Manual -12: its own depth is its ceiling, so -6 at 30 s and the
        // Clear at 40 s. Fix round 1, Minor 1: +10 was thin -- 3760 (before
        // the fix) left only 9 blocks over the 3751 actually needed:
        //   ceil(30000 / kBlockMs) + ceil(10000 / kBlockMs) = 2813 + 938 = 3751
        // +60 restores a real margin.
        const int blocks = (int) ((NotchController::kReleaseFirstMs
                                  + NotchController::kReleaseStepMs) / kBlockMs) + 60;
        for (int i = 0; i < blocks; ++i)
            pump (h, quiet.hop());
        const auto c = r.clears();
        ASSERT_EQ (c.size(), 1u);
        EXPECT_EQ (c[0].reason, NotchController::ClearReason::AutoRelease);
        EXPECT_GT (c[0].ageMs, NotchController::kAutoReleaseMs);
    }
    // WidthChange: lane-1 notch, narrow to mono.
    {
        Recorder r; StereoHarness h; h.controller.setEventSink (r.sink());
        ASSERT_TRUE (h.controller.setNotch (1, 2, 800.0, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.setWidth (1);
        h.controller.runOnce();
        const auto c = r.clears();
        ASSERT_EQ (c.size(), 1u);
        EXPECT_EQ (c[0].reason, NotchController::ClearReason::WidthChange);
        EXPECT_EQ (c[0].lane, 1); EXPECT_EQ (c[0].index, 2);
    }
    // PartialApplyUnwind: adoptPreset with lane -1 on a stereo slot, lane 1 forced to fail.
    {
        Recorder r; StereoHarness h; h.controller.setEventSink (r.sink());
        h.controller.failNextSetNotchOnLaneForTest (1);
        EXPECT_EQ (h.controller.adoptPreset (onePresetNotch (4, 1000.0)), 0);
        h.controller.runOnce();
        const auto c = r.clears();
        ASSERT_EQ (c.size(), 1u);
        EXPECT_EQ (c[0].reason, NotchController::ClearReason::PartialApplyUnwind);
        EXPECT_EQ (c[0].lane, 0); EXPECT_EQ (c[0].index, 4);
        NotchController::SnapshotBuffer snap; h.controller.copySnapshot (snap);
        EXPECT_EQ (snap.notchCount, 0u);
    }
}

// Spec test 9. Red if the sink is ever invoked with modelMutex_ held: the
// re-entrant clearNotch below would deadlock, and the try_lock would fail.
TEST (NotchControllerEvents, SinkRunsOutsideTheModelMutexAndMayReenter)
{
    Harness h;
    bool mutexWasFree = true;
    int  calls = 0;
    h.controller.setEventSink ([&] (const Ev& e)
    {
        ++calls;
        mutexWasFree = mutexWasFree && h.controller.modelMutexIsFreeForTest();
        if (e.kind == Ev::Kind::Set && e.index == 0)
            h.controller.clearNotch (0, 1);   // re-entrant policy call from inside the sink
    });
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    ASSERT_TRUE (h.controller.setNotch (0, 1, 2000.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.runOnce();   // delivers the two Sets; the re-entrant clear lands in the outbox
    h.controller.runOnce();   // delivers that Clear
    // mutexWasFree is the AND of all three observations (review round 1: the
    // original overwrote it on each call, so only the last one was checked).
    EXPECT_TRUE (mutexWasFree);
    EXPECT_EQ (calls, 3);
}

// Spec test 10. Red if events accumulate with no sink attached.
TEST (NotchControllerEvents, NoSinkMeansNoAccumulation)
{
    Harness h;
    for (int i = 0; i < 200; ++i)
    {
        ASSERT_TRUE (h.controller.setNotch (0, i % 16, 1000.0 + i, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.clearNotch (0, i % 16);
    }
    h.controller.runOnce();
    EXPECT_EQ (h.controller.pendingEventsForTest(), 0);
    EXPECT_EQ (h.controller.droppedEvents(), 0u);
}

// Cap. Red if the outbox grows past kMaxPendingEvents instead of dropping
// and counting.
TEST (NotchControllerEvents, OutboxDropsAndCountsPastTheCap)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    for (int i = 0; i < 100; ++i)
    {
        ASSERT_TRUE (h.controller.setNotch (0, i % 16, 1000.0 + i, 30.0, -12.0, NotchController::Origin::Manual));
        h.controller.clearNotch (0, i % 16);
    }
    EXPECT_EQ (h.controller.pendingEventsForTest(), NotchController::kMaxPendingEvents);
    EXPECT_EQ (h.controller.droppedEvents(), 200u - (std::uint64_t) NotchController::kMaxPendingEvents);
    h.controller.runOnce();
    EXPECT_EQ (r.events.size(), (std::size_t) NotchController::kMaxPendingEvents);
}

// Spec test 11. Red if stop() stops flushing what is still queued.
TEST (NotchControllerEvents, StopFlushesPendingEventsEvenWhenTheThreadNeverRan)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.stop (1000);
    ASSERT_EQ (r.events.size(), 1u);
    EXPECT_EQ (r.events[0].kind, Ev::Kind::Set);
}

// Review round 1, finding 5. Red if stop()'s flush stops at one pass: the
// sink re-enters clearNotch() while handling the Set, queuing a Clear that
// nothing would drain once the thread is already joined and gone.
TEST (NotchControllerEvents, StopDrainsEventsQueuedByAReentrantSinkDuringItsOwnFlush)
{
    Recorder r; Harness h;
    h.controller.setEventSink ([&] (const Ev& e)
    {
        r.events.push_back (e);
        if (e.kind == Ev::Kind::Set)
            h.controller.clearNotch (0, e.index);   // re-entrant, from inside stop()'s flush
    });
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.stop (1000);   // thread never started; flush must still drain the re-entrant Clear
    const auto c = r.clears();
    ASSERT_EQ (c.size(), 1u);
    EXPECT_EQ (c[0].index, 0);
}

// Lane S loose end (A-9). Red if a lane-1 notch skipped on a mono slot is
// not counted.
TEST (NotchControllerPreset, AdoptPresetCountsLaneOneNotchesSkippedOnAMonoSlot)
{
    StereoHarness h;
    h.controller.setWidth (1);
    PresetNotch onLaneOne; onLaneOne.index = 0; onLaneOne.freq = 1000.0; onLaneOne.Q = 30.0; onLaneOne.depthDB = -12.0; onLaneOne.lane = 1;
    PresetNotch onLaneZero = onLaneOne; onLaneZero.index = 1; onLaneZero.lane = 0;
    int skipped = -1;
    EXPECT_EQ (h.controller.adoptPreset ({ onLaneOne, onLaneZero }, &skipped), 1);
    EXPECT_EQ (skipped, 1);
}

// Spec test 7. Red if the detector's Set event stops carrying the frame it
// scored (ctx.now), the frame the rise axis compared against (ctx.ref, with
// its age), or the other lane's frame; or if the recorded axes stop
// multiplying to the recorded score.
TEST (NotchControllerEvents, DetectorPlacementCarriesTheScoredFrameAndTheScorersReference)
{
    Recorder r; StereoHarness h; h.controller.setEventSink (r.sink());
    h.controller.setDetectionActive (true);
    NoiseSource quietL; SineSource toneR;
    const auto cmds = warmThenDrive (h, quietL, toneR);
    ASSERT_FALSE (cmds.empty());

    const Ev* set = nullptr;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Set && e.hasScore) { set = &e; break; }
    ASSERT_NE (set, nullptr) << "no scored Set event reached the sink";

    EXPECT_EQ (set->lane, 1);
    EXPECT_EQ (set->confirmedLane, 1);
    EXPECT_EQ (set->origin, NotchController::Origin::Detector);
    EXPECT_GT (set->score, CandidateScorer::kConfirmScore);
    EXPECT_FLOAT_EQ (set->pNorm * set->rise * set->novelty * set->penalty * set->asymmetry, set->score);
    EXPECT_EQ (set->persistNeeded, NotchController::kPersistenceBlocks);
    EXPECT_FLOAT_EQ (set->thr, PeakinessAnalyzer::kDefaultThreshold);

    ASSERT_NE (set->ctx, nullptr);
    const auto& ctx = *set->ctx;
    EXPECT_EQ (ctx.bins, Detector::kNumBins);
    EXPECT_NEAR (ctx.binHz, kTestSr / Detector::kFftSize, 1e-9);

    // ctx.now IS the scored frame: its peak bin is the howl.
    const int bin = (int) std::lround (set->hz / ctx.binHz);
    int argmax = 0;
    for (int b = 1; b < Detector::kNumBins; ++b)
        if (ctx.now[(std::size_t) b] > ctx.now[(std::size_t) argmax]) argmax = b;
    EXPECT_EQ (argmax, bin);

    // ctx.ref is the frame the rise axis used (D-7): at least 0.45 x rise old,
    // and the rise it implies is the rise the score encodes (rNorm >= 0.7
    // means now/was >= 1.35).
    ASSERT_TRUE (ctx.hasRef);
    EXPECT_GE (ctx.refAgeMs, 0.45 * CandidateScorer::kDefaultRiseReferenceMs - 1e-6);
    EXPECT_LE (ctx.refAgeMs, CandidateScorer::kRiseHistoryWindowMs);
    EXPECT_GE (ctx.now[(std::size_t) bin], 1.35f * ctx.ref[(std::size_t) bin]);

    // The other lane was quiet: its frame is there and small at the howl bin.
    ASSERT_TRUE (ctx.hasOther);
    EXPECT_LT (ctx.other[(std::size_t) bin], 0.1f * ctx.now[(std::size_t) bin]);
}

// ===========================================================================
// Lane R (RING RISK readout, spec docs/spec-ring-risk.md §1, amendment A-R1..
// A-R4/A-R8). The snapshot carries the SAME confidence number the placement
// decision used -- not a second, independently-computed peakiness. These
// tests pin that identity, the validity gate, and the published threshold.
// ===========================================================================

TEST (NotchControllerRingRisk, InvalidBeforeAnyFrame)
{
    Harness h;
    h.controller.setDetectionActive (true);

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_FALSE (snap.ringRiskValid);      // GUI must read this as Unavailable
    EXPECT_FLOAT_EQ (snap.ringRiskScore, 0.0f);
}

TEST (NotchControllerRingRisk, PublishesThresholdSoTheGuiNeverHardcodesIt)
{
    Harness h;
    h.controller.setDetectionActive (true);
    NoiseSource quiet;
    for (int i = 0; i < 4; ++i)
        pump (h, quiet.hop());

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    // A-R3: the band line is the confirm score, published live -- score is a
    // 0..1 product, so the spec's 10.0 could never be crossed.
    EXPECT_FLOAT_EQ (snap.ringRiskThreshold, CandidateScorer::kConfirmScore);
}

TEST (NotchControllerRingRisk, ValidWithHistoryAndScoreCrossesThresholdOnThePlacingFrame)
{
    Harness h;
    h.controller.setDetectionActive (true);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_TRUE (snap.ringRiskValid);
    // The frame that confirmed a notch scored above kConfirmScore by
    // definition (NotchController.cpp: `if (score > kConfirmScore)`), so the
    // published number -- being that same `score` -- must read Critical.
    EXPECT_GT (snap.ringRiskScore, snap.ringRiskThreshold);
}

TEST (NotchControllerRingRisk, NoiseOnlyFramesAreValidAndReadLow)
{
    Harness h;
    h.controller.setDetectionActive (true);
    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    // Acceptance 3 (as amended): noise is not "unknown", it is "quiet".
    EXPECT_TRUE (snap.ringRiskValid);
    EXPECT_FLOAT_EQ (snap.ringRiskScore, 0.0f);
    EXPECT_LT (snap.ringRiskScore, 0.55f * snap.ringRiskThreshold);   // Low band
}

TEST (NotchControllerRingRisk, DetectionDisabledPublishesInvalidNotZero)
{
    Harness h;
    h.controller.setDetectionActive (true);
    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    {
        NotchController::SnapshotBuffer armed;
        h.controller.copySnapshot (armed);
        ASSERT_TRUE (armed.ringRiskValid);
    }

    // A-R4: a disarmed detector scores nothing, so the chip must go back to
    // N/A rather than reassure the operator with a "Low" it did not measure.
    h.controller.setDetectionActive (false);
    pump (h, quiet.hop());

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_FALSE (snap.ringRiskValid);
    EXPECT_FLOAT_EQ (snap.ringRiskScore, 0.0f);
}

TEST (NotchControllerRingRisk, ScoreIsMaxOverBothLanesOfTheSlot)
{
    // A-R8: one score per SLOT after lane S. A howl on lane 1 only must still
    // raise the slot's readout -- max over lanes, not lane 0's number.
    StereoHarness h;
    h.controller.setDetectionActive (true);

    NoiseSource quietL, quietR;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());

    NotchController::SnapshotBuffer beforeTone;
    h.controller.copySnapshot (beforeTone);
    ASSERT_TRUE (beforeTone.ringRiskValid);

    SineSource tone;
    float peak = 0.0f;
    for (int i = 0; i < 8; ++i)
    {
        pumpStereo (h, quietL.hop(), tone.hop());   // howl on lane 1 only
        NotchController::SnapshotBuffer snap;
        h.controller.copySnapshot (snap);
        peak = std::max (peak, snap.ringRiskScore);
    }
    EXPECT_GT (peak, 0.0f);
}

// A-R4's reset boundaries. The scorer is deliberately NOT reset (lane D
// removed that; resetting it would move notches), so after a device/SR change
// its rise history and baseline EMAs still hold old-rate magnitudes at bin
// indices that now map to different frequencies. The readout must say N/A
// rather than publish a number computed from them -- "khong tran an sai".
TEST (NotchControllerRingRisk, SampleRateChangeInvalidatesUntilTheNextBlock)
{
    Harness h;
    h.controller.setDetectionActive (true);
    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    ASSERT_TRUE (snap.ringRiskValid);

    h.controller.setSampleRate (44100.0);

    // First frame at the new rate: nothing has been committed since the reset,
    // so the chip goes back to N/A.
    pump (h, quiet.hop());
    h.controller.copySnapshot (snap);
    EXPECT_FALSE (snap.ringRiskValid);
    EXPECT_FLOAT_EQ (snap.ringRiskScore, 0.0f);

    // One committed block later it is a measurement again.
    pump (h, quiet.hop());
    h.controller.copySnapshot (snap);
    EXPECT_TRUE (snap.ringRiskValid);
}

TEST (NotchControllerRingRisk, WidthChangeInvalidatesUntilTheNextBlock)
{
    Harness h;
    h.controller.setDetectionActive (true);
    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    ASSERT_TRUE (snap.ringRiskValid);

    // Legal here: the detector thread was never started, these harnesses drive
    // runOnce() by hand. This is also the call MainComponent's onAfterRestart
    // hook makes for every slot after EVERY engine restart, so it is the path
    // a real device or sample-rate change takes.
    h.controller.setWidth (1);

    pump (h, quiet.hop());
    h.controller.copySnapshot (snap);
    EXPECT_FALSE (snap.ringRiskValid);
    EXPECT_FLOAT_EQ (snap.ringRiskScore, 0.0f);

    pump (h, quiet.hop());
    h.controller.copySnapshot (snap);
    EXPECT_TRUE (snap.ringRiskValid);
}

// ===========================================================================
// Lane G (gain-aware notch), spec docs/superpowers/specs/
// 2026-09-06-gain-aware-notch-design.md. The ladder, its clamps, and the
// retune command path.
// ===========================================================================

// RED IF the effective ladder stops being "the rungs shallower than the
// ceiling, then the ceiling itself" (Q13). The shipped presets/Music.json
// asks for -10, which is not a multiple of 6: v2's quantise-to-a-rung rule
// would have capped Music at -6, i.e. 4 dB SHALLOWER than 1.1.3, silently.
TEST (NotchControllerLadder, TheEffectiveLadderEndsOnTheCeilingItself)
{
    using NC = NotchController;
    // Ceiling -24: the whole fixed ladder.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -6.0, -24.0), -12.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-12.0, -24.0), -18.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-18.0, -24.0), -24.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-24.0, -24.0), -24.0);   // saturates

    // Ceiling -10 (presets/Music.json): -6 -> -10, and -10 is the end.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -6.0, -10.0), -10.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-10.0, -10.0), -10.0);

    // Ceiling -13.7: -6 -> -12 -> -13.7.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -6.0, -13.7), -12.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-12.0, -13.7), -13.7);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-13.7, -13.7), -13.7);

    // Ceiling -18 (presets/Speech.json): -6 -> -12 -> -18.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -6.0, -18.0), -12.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-12.0, -18.0), -18.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-18.0, -18.0), -18.0);

    // Ceiling -6: there is no rung shallower than -6, so the ladder is one
    // rung long and a notch there NEVER deepens.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -6.0,  -6.0),  -6.0);

    // An odd Manual depth resolves in the direction of travel, still capped.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -3.0, -24.0),  -6.0);
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb ( -9.0, -24.0), -12.0);

    // Fix-round 1 (review finding "Important 2"): DEEPER than the ceiling --
    // e.g. the slider dropped after a notch already stood past its new
    // ceiling -- steps the notch back UP to the ceiling, not further down.
    // This is a step in the SHALLOWER direction; the stale header comment
    // claimed the function was a no-op here.
    EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-24.0, -10.0), -10.0);
}

// RED IF a release step can jump more than one rung, or past -6. Q13 needs no
// ceiling here: releasing is always toward a FIXED rung, and the two off-rung
// depths the ladder can hold -- an odd ceiling, an odd Preset/Manual depth --
// both resolve to the nearest fixed rung above them.
TEST (NotchControllerLadder, NextShallowerIsExactlyOneRungAndStopsAtMinus6)
{
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-24.0), -18.0);
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-18.0), -12.0);
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-12.0),  -6.0);
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-6.0),   -6.0);
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-13.7), -12.0);
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-10.0),  -6.0);  // Music's ceiling
    EXPECT_DOUBLE_EQ (NotchController::nextShallowerRungDb (-9.0),   -6.0);  // odd preset depth
}

// RED IF setNotchImpl stops re-initialising the ladder fields when a slot is
// REUSED (B-2). pushClearLocked only lowers `active`, so without this a manual
// -6 dB notch inherits the -24 dB deepestDb of the previous tenant and gets
// reclamped to -24 on its first reinforce.
// Fix-round 1 (review finding "Important 1"): the original test covered only
// deepestDb/quietMs of the five ladder fields setNotchImpl re-initialises.
// Extended to cover all five, and to place the FIRST tenant as Manual with a
// non-NaN raw ceiling and non-zero live time, so a stale ceilingDb or a stale
// stageChangedAtMs would be PROVABLY stale rather than accidentally still
// matching the reused slot's own values.
TEST (NotchControllerLadder, ReusingASlotResetsEveryLadderField)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -24.0,
                                        NotchController::Origin::Manual));
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -24.0);
    EXPECT_DOUBLE_EQ (h.controller.rawCeilingDbForTest (0, 0), -24.0);
    EXPECT_EQ (h.controller.releasedStepsForTest (0, 0), 0);
    const double firstStageChangedAtMs = h.controller.stageChangedAtMsForTest (0, 0);

    // Move live time forward so the second placement's stageChangedAtMs is
    // measurably different from the first, if it were left stale.
    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 50; ++i) {          // 50 * 5 ms = 250 ms of live time
        h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }

    h.controller.clearNotch (0, 0, NotchController::ClearReason::Manual);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -6.0,
                                        NotchController::Origin::Detector));

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -6.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -6.0);
    EXPECT_DOUBLE_EQ (h.controller.quietMsForTest (0, 0),    0.0);
    EXPECT_EQ (h.controller.releasedStepsForTest (0, 0), 0);
    EXPECT_TRUE (std::isnan (h.controller.rawCeilingDbForTest (0, 0)))
        << "Manual's -24 ceiling must not survive into a reused Detector slot";
    EXPECT_GT (h.controller.stageChangedAtMsForTest (0, 0), firstStageChangedAtMs)
        << "stageChangedAtMs must be re-stamped to the CURRENT live time, not left stale";
}

// RED IF ceilingDbFor stops resolving to the LIVE slider for a Detector
// notch, or starts doing the same for anything else (Q8). ceilingDbForTest is
// the RESOLVED ceiling ceilingDbFor(n) returns; rawCeilingDbForTest is the
// stored field itself -- NaN for Detector, the notch's own depth otherwise.
TEST (NotchControllerLadder, CeilingIsTheSliderForDetectorAndOwnDepthForPresetAndManual)
{
    Harness h;

    // Detector: raw field is the NaN sentinel, and the resolved ceiling
    // tracks the LIVE slider -- including a change made AFTER placement.
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    EXPECT_TRUE (std::isnan (h.controller.rawCeilingDbForTest (0, 0)));
    EXPECT_DOUBLE_EQ (h.controller.ceilingDbForTest (0, 0), h.controller.getNotchDepthDb());

    h.controller.setNotchDefaults (30.0, -12.0);
    EXPECT_DOUBLE_EQ (h.controller.ceilingDbForTest (0, 0), -12.0)
        << "a Detector notch's ceiling must follow the slider even after placement";

    // Manual: raw and resolved are both the caller's own depth; moving the
    // slider afterward changes NOTHING for it.
    ASSERT_TRUE (h.controller.setNotch (0, 1, 1000.0, 30.0, -9.0,
                                        NotchController::Origin::Manual));
    EXPECT_DOUBLE_EQ (h.controller.rawCeilingDbForTest (0, 1), -9.0);
    EXPECT_DOUBLE_EQ (h.controller.ceilingDbForTest (0, 1), -9.0);
    h.controller.setNotchDefaults (30.0, -6.0);
    EXPECT_DOUBLE_EQ (h.controller.ceilingDbForTest (0, 1), -9.0)
        << "a Manual notch's ceiling must NOT drag with the slider";

    // Preset (adopted): same as Manual -- its own depth, slider-independent.
    PresetNotch p;
    p.index = 2; p.freq = 1000.0; p.Q = 30.0; p.depthDB = -15.0;
    ASSERT_EQ (h.controller.adoptPreset ({ p }), 1);
    EXPECT_DOUBLE_EQ (h.controller.rawCeilingDbForTest (0, 2), -15.0);
    EXPECT_DOUBLE_EQ (h.controller.ceilingDbForTest (0, 2), -15.0);
}

// RED IF `activeForTest` is implemented as "depthDB < 0" (B-3). It must read
// ModelNotch::active, because pushClearLocked lowers ONLY `active` -- the
// Clear event reads n.depthDB, so the depth is deliberately retained on a
// cleared slot. Every liveness probe in Tasks 5-8 is built on this accessor;
// a depth-based one would report every slot that has ever held a notch as
// still active, and the room-memory and ceiling tests would pass while
// asserting nothing.
//
// DO NOT "fix" this by zeroing depthDB in pushClearLocked: that would empty
// the Clear event's depth field, which lane D writes into every notch_clear.
TEST (NotchControllerLadder, ActiveForTestReadsTheFlagNotTheRetainedDepth)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    EXPECT_TRUE (h.controller.activeForTest (0, 0));

    h.controller.clearNotch (0, 0, NotchController::ClearReason::Manual);
    EXPECT_FALSE (h.controller.activeForTest (0, 0));
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -18.0)
        << "a Clear must NOT zero depthDB -- the Clear event reads it";

    EXPECT_FALSE (h.controller.activeForTest (0, 1));   // never touched
}

// RED IF the -24 clamp (Q12) is narrowed to Origin::Detector, or dropped.
// Invariant 1 must hold for EVERY origin, or a hand-edited preset puts a
// -40 dB cut on a PA.
TEST (NotchControllerLadder, DepthDeeperThanMinus24IsClampedForEveryOrigin)
{
    const NotchController::Origin origins[] = { NotchController::Origin::Detector,
                                                NotchController::Origin::Preset,
                                                NotchController::Origin::Manual,
                                                NotchController::Origin::Soundcheck };
    for (auto origin : origins)
    {
        Harness h;
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -40.0, origin));
        h.controller.runOnce();

        NotchCommand cmd {};
        ASSERT_EQ (h.commands.read (&cmd, 1), 1u);
        EXPECT_FLOAT_EQ (cmd.depthDB, -24.0f);
        EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -24.0);
        EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -24.0);
    }
}

// RED IF adoptPreset stops clamping. A file is not a trusted source.
TEST (NotchControllerLadder, AdoptPresetClampsADeepPresetNotch)
{
    Harness h;
    PresetNotch p;
    p.index = 0; p.freq = 1000.0; p.Q = 30.0; p.depthDB = -40.0;
    EXPECT_EQ (h.controller.adoptPreset ({ p }), 1);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -24.0);
}

// RED IF pushRetuneLocked stops sending the STORED freq/Q (m-2). Sending the
// live notchQ_ instead would differ by a bit whenever the operator had moved
// the Q slider after placement, and NotchChain would take the reset path --
// which is a click.
TEST (NotchControllerLadder, RetuneResendsTheStoredFrequencyAndQ)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 2, 987.0, 17.5, -6.0,
                                        NotchController::Origin::Detector));
    h.controller.setNotchDefaults (44.0, -24.0);   // move the live defaults away
    ASSERT_TRUE (h.controller.retuneForTest (0, 2, -12.0,
                                             NotchController::RetuneReason::Deepen));
    h.controller.runOnce();

    NotchCommand cmd {};
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);   // the original Set
    ASSERT_EQ (h.commands.read (&cmd, 1), 1u);   // the retune
    EXPECT_EQ (cmd.type, NotchCommandType::Set);
    EXPECT_EQ (cmd.channel, 0);
    EXPECT_EQ (cmd.index, 2);
    EXPECT_FLOAT_EQ (cmd.frequency, 987.0f);
    EXPECT_FLOAT_EQ (cmd.Q, 17.5f);
    EXPECT_FLOAT_EQ (cmd.depthDB, -12.0f);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 2), -12.0);
}

// RED IF a retune overwrites lockedAtMs (B-1). lockedAtMs is the age label
// lane D writes into every notch_clear; resetting it on each retune would make
// a notch that lived 40 s report a 300 ms life.
TEST (NotchControllerLadder, RetuneDoesNotResetTheNotchesAge)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -6.0,
                                        NotchController::Origin::Detector));

    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 200; ++i) {          // 200 * 5 ms = 1 s of live time
        h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    ASSERT_TRUE (h.controller.retuneForTest (0, 0, -12.0,
                                             NotchController::RetuneReason::Deepen));
    ASSERT_TRUE (h.controller.retuneForTest (0, 0, -18.0,
                                             NotchController::RetuneReason::Deepen));
    h.controller.clearNotch (0, 0, NotchController::ClearReason::Manual);
    h.controller.runOnce();

    const auto clears = r.clears();
    ASSERT_EQ (clears.size(), 1u);
    EXPECT_GT (clears[0].ageMs, 900.0) << "age was measured from the last retune, not the placement";
    EXPECT_FLOAT_EQ (clears[0].depthDb, -18.0f);   // the depth it was actually running
}

// RED IF a Retune event stops carrying where it came FROM, or starts
// allocating a SpectralContext (a retune is not a placement decision).
TEST (NotchControllerLadder, RetuneEventCarriesFromDepthReasonAndNoContext)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 1, 1000.0, 30.0, -6.0,
                                        NotchController::Origin::Detector));
    ASSERT_TRUE (h.controller.retuneForTest (0, 1, -18.0,
                                             NotchController::RetuneReason::Reclamp));
    h.controller.runOnce();

    const Ev* ret = nullptr;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Retune) { ret = &e; break; }
    ASSERT_NE (ret, nullptr);
    EXPECT_EQ (ret->lane, 0);
    EXPECT_EQ (ret->index, 1);
    EXPECT_FLOAT_EQ (ret->fromDepthDb, -6.0f);
    EXPECT_FLOAT_EQ (ret->depthDb, -18.0f);
    EXPECT_EQ (ret->retuneReason, NotchController::RetuneReason::Reclamp);
    EXPECT_EQ (ret->origin, NotchController::Origin::Detector);
    EXPECT_EQ (ret->ctx, nullptr);
    EXPECT_FALSE (ret->hasScore);
}

// RED IF pushRetuneLocked drops any of its five validate-before-send
// predicates, or the -24 floor. A refused retune must change NOTHING.
TEST (NotchControllerLadder, RetuneRefusesOutOfRangeDepthAndChangesNothing)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Detector));
    h.controller.runOnce();
    NotchCommand drained {};
    while (h.commands.read (&drained, 1) == 1) {}

    EXPECT_FALSE (h.controller.retuneForTest (0, 0,  +3.0,
                                              NotchController::RetuneReason::Deepen));
    EXPECT_FALSE (h.controller.retuneForTest (0, 0, -40.0,
                                              NotchController::RetuneReason::Deepen));
    EXPECT_FALSE (h.controller.retuneForTest (0, 5, -18.0,   // slot not active
                                              NotchController::RetuneReason::Deepen));
    h.controller.runOnce();

    EXPECT_EQ (h.commands.getAvailableRead(), 0u);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);
}

// RED IF the snapshot stops carrying deepestDb -- savePreset (Q11) reads it,
// and a notch resting at -6 while the room needed -18 would be saved as -6.
TEST (NotchControllerLadder, SnapshotCarriesTheDeepestDepthTheNotchEverHeld)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    ASSERT_TRUE (h.controller.retuneForTest (0, 0, -6.0,
                                             NotchController::RetuneReason::Release));

    std::vector<float> hop (512, 0.25f);
    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    ASSERT_EQ (snap.notchCount, 1u);
    EXPECT_FLOAT_EQ (snap.notches[0].depthDB,   -6.0f);
    EXPECT_FLOAT_EQ (snap.notches[0].deepestDb, -18.0f);
    EXPECT_FALSE (snap.releaseFrozen);
}

// ===========================================================================
// Lane G Task 5: the PLACEMENT depth. A confirmed candidate starts on the
// shallowest rung the policy allows, not at the slider's depth.
// ===========================================================================

// RED IF placement goes back to reading notchDepthDb_ directly. A howl that
// creeps up gets the SHALLOWEST rung -- the whole point of lane G is that a
// room which only needs 6 dB does not lose 18 dB of tone (spec 4.3 step 1).
TEST (NotchControllerLadder, ASlowlyRisingHowlIsPlacedAtMinusSix)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    h.controller.setDetectionActive (true);

    const int slot = primeAndPlaceSlowly (h);
    ASSERT_GE (slot, 0) << "the slow ramp never confirmed inside 150 blocks -- "
                           "RAISE kRampStartAmp (still below the noise floor) "
                           "or steepen gainDbPerMs so peakiness clears the "
                           "confirm threshold sooner (B-5)";
    EXPECT_TRUE (h.controller.activeForTest (0, slot));
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -6.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -6.0);

    // B-5: prove the FIXTURE is the thing being tested, not an accident. -6 is
    // also what a fixture that barely confirmed on some other axis would
    // produce, so pin the raw rise inside the band the derivation aimed at:
    // >= 1.5 saturates rNorm (so the confirm was earned on rise), < 2.0 is why
    // the steep-rise branch did not fire. `rise` on the event is rNorm, which
    // saturates at 1.5 and cannot make this distinction -- riseRatio is the
    // raw ratio added in Task 3.
    //
    // riseRatio is only MEANINGFUL once the scorer has history at least
    // 112.5 ms deep: with no history at all the rise branch returns rNorm 1
    // and riseRatio 1.0 by definition, which would read as "too slow" here.
    // kWarmupBlocks is 64 blocks of noise, so by the time anything can be
    // confirmed the history is ~683 ms deep and this assertion is comparing
    // two real frames.
    const Ev* set = nullptr;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Set) { set = &e; break; }
    ASSERT_NE (set, nullptr);
    EXPECT_GE (set->riseRatio, 1.5f)
        << "the ramp was too slow to confirm on rise (or the fixture confirmed "
           "before the scorer had 112.5 ms of history)";
    EXPECT_LT (set->riseRatio, NotchController::kSteepRiseRatio)
        << "the confirm landed while the 117 ms-old reference frame was still "
           "a NOISE frame, so this measured a tone-vs-noise ratio, not "
           "tone-vs-tone: LOWER kRampStartAmp (start further below the noise "
           "floor), or add a few tone-only priming blocks before the "
           "reference ages past the onset (B-5) -- raising it walks further "
           "into the failing region, it does not fix it";
}

// RED IF the steep-rise jump stops firing (spec 4.3 step 2, Q6). SineSource
// switches a full-scale tone on in one block, so riseRatio >> 2.0.
TEST (NotchControllerLadder, ASteeplyRisingHowlIsPlacedAtMinusTwelve)
{
    Harness h;
    h.controller.setDetectionActive (true);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -12.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -12.0);
}

// RED IF the ceiling stops clamping the STARTING depth (spec 4.3 step 4). A
// ceiling of -6 must never let even a steep rise place at -12.
TEST (NotchControllerLadder, ACeilingOfMinusSixCapsEvenASteepRise)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -6.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -6.0);
}

// RED IF the ceiling is quantised down to a fixed rung instead of BEING the
// last rung (Q13). presets/Music.json ships depth -10; under v2's
// `ceilingRungDb` a steep rise there would have placed at -6, i.e. 4 dB
// shallower than 1.1.3, with nothing in the release note saying so.
TEST (NotchControllerLadder, AnOffRungCeilingIsItselfTheDeepestPlacement)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -10.0);   // the shipped Music ceiling

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));   // steep: wants -12
    ASSERT_GE (slot, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -10.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -10.0);
}

// RED IF a Soundcheck placement is dragged onto the ladder. KD-7 exempts
// soundcheck notches from every part of lane G: they are placed at the depth
// the operator asked for, on the first block, and never move.
TEST (NotchControllerLadder, SoundcheckPlacesAtTheFullSliderDepth)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -18.0);
    h.controller.startSoundcheck();

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -18.0);
}

// RED IF a Detector notch stops following the LIVE slider, or a Preset notch
// starts following it (Q8). Detector: ceilingDb is NaN. Preset: its own depth.
TEST (NotchControllerLadder, PresetNotchesKeepTheirOwnCeilingAndDetectorNotchesFollowTheSlider)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Preset));
    // A preset notch is placed AT its own depth and that depth is its ceiling:
    // deepestDb equals it, so nothing below can deepen past it.
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -12.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -12.0);

    // The preset's OWN Set is still in the command ring. primeAndPlace reads
    // the first command it finds once two are available, so leaving it there
    // makes the helper report the preset's index 0 as the detector's slot --
    // and the test then asserts the preset's depth against the detector's
    // policy. Flush it out before the detector is allowed to place.
    h.controller.runOnce();
    NotchCommand presetCmd {};
    while (h.commands.read (&presetCmd, 1) == 1)
        ;

    h.controller.setDetectionActive (true);
    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    ASSERT_NE (slot, 0);
    // The detector notch obeys the -24 slider, so the steep-rise rung stands.
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -12.0);
}

// === Task 6: the deepen ladder inside the reinforce loop ==================
//
// M-3: the harness writes the RAW tone into h.tap, so nothing applies the
// notch chain between the tone and the Detector -- the analyser sees the
// UN-notched spectrum on every frame. The Q7 claim "the ladder stops at the
// first rung that quiets the bin" therefore cannot be observed here and no
// test below pretends it can: in this fixture the bin never goes quiet and the
// ladder always climbs to the ceiling. That claim belongs in the tester notes.
//
// M-A: the reclamp branch is implemented in this task but has no red test in
// it. Only the release ladder of task 7 can put releasedSteps above 0 through
// the real path, and faking it through retuneForTest cannot work (that seam
// writes depthDB and nothing else). AReturningHowlReclampsImmediatelyToDeepestDb
// lives in task 7.

// RED IF the ladder stops climbing while the bin is still over threshold
// (spec 4.4), OR if a rung fires implausibly late (fix-round 1, M-4: the
// gate was bounded from below only, so a regression that fired the deepen
// gate every OTHER opportunity, or stalled for seconds, still passed). The
// tone never stops, so every 300 ms of live time buys one rung until the
// ceiling's rung is reached, and no rung should take anywhere near double
// that -- measured value is 309.3 ms against a 300 ms gate.
TEST (NotchControllerLadder, AContinuingHowlDeepensOneRungPer300ms)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);   // ceiling: the whole ladder

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -12.0);
    const double placedAt = h.controller.liveMsForTest();

    // Keep the howl going. 300 ms is ~29 blocks of 10.667 ms.
    SineSource tone;
    double sawMinus18At = -1.0, sawMinus24At = -1.0;
    for (int i = 0; i < 200; ++i)
    {
        pump (h, tone.hop());
        const double d = h.controller.depthDbForTest (0, slot);
        if (sawMinus18At < 0.0 && d <= -18.0) sawMinus18At = h.controller.liveMsForTest();
        if (sawMinus24At < 0.0 && d <= -24.0) sawMinus24At = h.controller.liveMsForTest();
    }

    ASSERT_GT (sawMinus18At, 0.0) << "the ladder never reached -18";
    ASSERT_GT (sawMinus24At, 0.0) << "the ladder never reached -24";
    EXPECT_GE (sawMinus18At - placedAt, NotchController::kDeepenAfterMs);
    EXPECT_GE (sawMinus24At - sawMinus18At, NotchController::kDeepenAfterMs);
    EXPECT_LT (sawMinus18At - placedAt, 2.0 * NotchController::kDeepenAfterMs)
        << "the first rung fired implausibly late";
    EXPECT_LT (sawMinus24At - sawMinus18At, 2.0 * NotchController::kDeepenAfterMs)
        << "the second rung fired implausibly late";
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -24.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -24.0);
}

// RED IF the ceiling stops capping the climb. A ceiling of -18 must leave the
// notch at -18 no matter how long the howl continues.
TEST (NotchControllerLadder, DeepeningStopsAtTheCeilingRung)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -18.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);

    SineSource tone;
    for (int i = 0; i < 300; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -18.0);
}

// RED IF the last step of the climb is a full 6 dB past an off-rung ceiling,
// or is quantised away (Q13). Ceiling -13.7: the effective ladder is
// -6 -> -12 -> -13.7, so the final step is 1.7 dB, not 6.
TEST (NotchControllerLadder, TheLastStepLandsExactlyOnAnOffRungCeiling)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -13.7);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));   // steep -> -12, capped -12
    ASSERT_GE (slot, 0);
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -12.0);

    SineSource tone;
    for (int i = 0; i < 300; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -13.7);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -13.7);
}

// RED IF a ceiling of -6 stops meaning "never deepen" (spec 4.1, Q13: the
// effective ladder for ceiling -6 is one rung long).
//
// M-2: this must NOT assert "no Set at all". Detection stays armed -- and it
// has to, because reinforcement lives behind the same gate -- so the armed
// detector keeps confirming and placing FRESH notches on the still-ringing
// lane, each of which is a legitimate Set on a DIFFERENT index. Assert on the
// depth of THIS slot, and on commands carrying this (lane, index) only.
TEST (NotchControllerLadder, ACeilingOfMinusSixNeverDeepens)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -6.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    NotchCommand drained {};
    while (h.commands.read (&drained, 1) == 1) {}

    SineSource tone;
    int retunesOfThisSlot = 0;
    for (int i = 0; i < 300; ++i)
    {
        pump (h, tone.hop());
        while (h.commands.read (&drained, 1) == 1)
            if (drained.type == NotchCommandType::Set
                && drained.channel == 0 && drained.index == slot)
                ++retunesOfThisSlot;
    }

    EXPECT_TRUE (h.controller.activeForTest (0, slot));
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -6.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -6.0);
    EXPECT_EQ (retunesOfThisSlot, 0) << "a ceiling of -6 emitted a retune";
}

// RED IF a Preset notch is dragged down the ladder (spec 4.3): the file said
// what it wanted, and its own depth is its ceiling.
TEST (NotchControllerLadder, APresetNotchNeverDeepens)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);
    // Place it exactly on the tone SineSource produces (bin 43 = 1007.8125 Hz)
    // so the reinforce loop finds it every frame.
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -12.0,
                                        NotchController::Origin::Preset));

    NoiseSource quiet;
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    SineSource tone;
    for (int i = 0; i < 200; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);
}

// RED IF a LINKED pair can end up on different rungs on ANY block of the
// climb -- not just after both have saturated at the ceiling. Both lanes are
// reinforced from the same frame, so they must climb together in lockstep;
// asserting only once at the end (fix-round 1, I-2) would let a lane that
// climbed a rung late still pass once the other one caught up.
TEST (NotchControllerLadder, LinkedLanesStayOnTheSameRung)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    NoiseSource quietL, quietR; quietR.rng.seed (999u);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());

    // B-3: activeForTest, not "depthDbForTest < 0" -- a cleared slot keeps its
    // depth, so a depth probe would compare two dead lanes and pass on nothing.
    SineSource toneL, toneR;
    for (int i = 0; i < 200; ++i)
    {
        pumpStereo (h, toneL.hop(), toneR.hop());
        for (int s = 0; s < NotchController::kSlots; ++s)
        {
            EXPECT_EQ (h.controller.activeForTest (0, s), h.controller.activeForTest (1, s))
                << "block " << i << " index " << s << " active mismatch";
            if (h.controller.activeForTest (0, s) && h.controller.activeForTest (1, s))
            {
                EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, s),
                                  h.controller.depthDbForTest (1, s))
                    << "block " << i << " index " << s;
                EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, s),
                                  h.controller.deepestDbForTest (1, s))
                    << "block " << i << " index " << s;
            }
        }
    }

    // Final state, kept as the original assertion.
    bool sawAny = false;
    for (int i = 0; i < NotchController::kSlots; ++i)
        if (h.controller.activeForTest (0, i) && h.controller.activeForTest (1, i))
        {
            sawAny = true;
            EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, i),
                              h.controller.depthDbForTest (1, i)) << "index " << i;
            EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, i),
                              h.controller.deepestDbForTest (1, i)) << "index " << i;
        }
    EXPECT_TRUE (sawAny) << "no linked pair was ever placed";
}

// RED IF an INDEP placement touches the other lane (spec 5.3, INDEP leaves the
// other lane alone), on ANY block of the climb -- not just after 200 blocks.
// A stray placement or deepen on the quiet lane that got cleared before the
// end would have passed the old end-of-loop-only check (fix-round 1, I-2).
// The right lane is quiet throughout, so nothing may appear on it at any rung.
TEST (NotchControllerLadder, IndepLeavesTheOtherLaneUntouchedThroughTheWholeClimb)
{
    StereoHarness h;
    h.controller.setLinked (false);
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    NoiseSource quietL, quietR; quietR.rng.seed (999u);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());

    SineSource toneL;
    for (int i = 0; i < 200; ++i)
    {
        pumpStereo (h, toneL.hop(), quietR.hop());
        for (int s = 0; s < NotchController::kSlots; ++s)
            EXPECT_FALSE (h.controller.activeForTest (1, s))
                << "block " << i << " index " << s
                << ": INDEP placed or deepened on the quiet lane";
    }

    bool sawLeft = false;
    for (int i = 0; i < NotchController::kSlots; ++i)
    {
        sawLeft |= h.controller.activeForTest (0, i);
        EXPECT_FALSE (h.controller.activeForTest (1, i))
            << "INDEP placed or deepened on the quiet lane, index " << i;
    }
    EXPECT_TRUE (sawLeft) << "the ringing lane never placed anything";
}

// RED IF a Soundcheck notch is deepened or reclamped (KD-7).
TEST (NotchControllerLadder, SoundcheckNotchesNeverDeepen)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    h.controller.startSoundcheck();

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    const double placed = h.controller.depthDbForTest (0, slot);

    SineSource tone;
    for (int i = 0; i < 200; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), placed);
}

// RED IF any command the ladder emits leaves the validated depth window
// (spec 4.10 invariant 1 / Q12). A small fuzz over the ceiling, on-rung and
// off-rung: every Set this controller emits -- placement OR retune -- must
// carry a depth in [-24, 0]. Deliberately cheap: the point is that no ceiling
// value can talk the ladder past the floor, not a statistical claim.
TEST (NotchControllerLadder, NoEmittedSetEverLeavesTheValidDepthWindow)
{
    const double ceilings[] = { -6.0, -7.3, -10.0, -13.7, -18.0, -23.4, -24.0 };
    for (double ceiling : ceilings)
    {
        Harness h;
        h.controller.setDetectionActive (true);
        h.controller.setNotchDefaults (30.0, ceiling);

        NoiseSource quiet;
        for (int i = 0; i < kWarmupBlocks; ++i)
            pump (h, quiet.hop());

        SineSource tone;
        int sets = 0;
        for (int i = 0; i < 120; ++i)
        {
            pump (h, tone.hop());
            NotchCommand cmd {};
            while (h.commands.read (&cmd, 1) == 1)
                if (cmd.type == NotchCommandType::Set)
                {
                    ++sets;
                    EXPECT_LE (cmd.depthDB, 0.0f)   << "ceiling " << ceiling;
                    EXPECT_GE (cmd.depthDB, -24.0f) << "ceiling " << ceiling;
                }
        }
        EXPECT_GT (sets, 0) << "ceiling " << ceiling << ": nothing was ever emitted";
    }
}

// ===========================================================================
// Task 7 -- the release ladder in runOnce step 3 (spec 4.5, Q3/Q4/Q9).
// ===========================================================================

// RED IF the release cliff comes back, or the rung timings move (spec 4.5,
// Q3). This is the headline behaviour of the whole lane.
TEST (NotchControllerLadder, ReleaseWalksTheLadderAt30sThen10sPerRung)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;

    pumpQuietFor (h, quiet, 29000.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -18.0) << "released early";

    pumpQuietFor (h, quiet, 1500.0);                     // past 30 s
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);

    pumpQuietFor (h, quiet, 9000.0);                     // 9 s into the 10 s rung
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);

    pumpQuietFor (h, quiet, 1500.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -6.0);

    pumpQuietFor (h, quiet, 10500.0);                    // 30 + 10 + 10 = 50 s: Clear
    // B-3: the notch is GONE, which is `active == false`. depthDbForTest still
    // reads -6 here, because pushClearLocked deliberately leaves depthDB alone
    // for the Clear event to read.
    EXPECT_FALSE (h.controller.activeForTest (0, 0))
        << "the notch never cleared at the bottom of the ladder";

    NotchController::SnapshotBuffer snap;
    std::vector<float> hop (512, 0.05f);
    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();
    h.controller.copySnapshot (snap);
    EXPECT_EQ (snap.notchCount, 0u);
}

// RED IF the final rung stops emitting a real Clear with the AutoRelease
// reason -- lane D's label and Task 8's room-memory write both hang off it.
TEST (NotchControllerLadder, TheBottomOfTheLadderClearsWithAutoRelease)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 52000.0);   // from -18: 30 + 10 + 10 = 50 s, +2 s margin
    h.controller.runOnce();

    const auto clears = r.clears();
    ASSERT_EQ (clears.size(), 1u);
    EXPECT_EQ (clears[0].reason, NotchController::ClearReason::AutoRelease);
    EXPECT_FLOAT_EQ (clears[0].depthDb, -6.0f) << "cleared from the wrong rung";

    int retunes = 0;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Retune
            && e.retuneReason == NotchController::RetuneReason::Release)
            ++retunes;
    EXPECT_EQ (retunes, 2) << "-18 -> -12 -> -6 is two release steps";
}

// RED IF the freeze stops working (spec 4.5 step 1, Q4). A tense room must not
// have its notches wound back under it. There is deliberately NO time cap.
TEST (NotchControllerLadder, RingRiskAtRisingFreezesTheReleaseClock)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    h.controller.setRingRiskOverrideForTest (
        std::make_pair (true, NotchController::kRiskFreezeFraction
                              * CandidateScorer::kConfirmScore));

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 90000.0);   // three times the first rung's time
    EXPECT_DOUBLE_EQ (h.controller.quietMsForTest (0, 0), 0.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -18.0);

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_TRUE (snap.releaseFrozen) << "the frozen flag was never published (Q9)";

    // Room calms: the clock starts again from where it was, which is 0.
    h.controller.setRingRiskOverrideForTest (std::nullopt);
    pumpQuietFor (h, quiet, 31000.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);
}

// Fix round 1, Important 1. RED IF the frozen flag survives a dead tap
// (M-6). A dead tap is not a frozen clock; it is no clock at all -- a GUI or
// a log reader must never be told "frozen" about a slot with no audio.
TEST (NotchControllerLadder, ReleaseFrozenGoesFalseWhenTheTapDies)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));
    h.controller.setRingRiskOverrideForTest (
        std::make_pair (true, NotchController::kRiskFreezeFraction
                              * CandidateScorer::kConfirmScore));

    // One live block: the tap is alive and RING RISK reads RISING, so the
    // clock freezes immediately (same shape as RingRiskAtRisingFreezesThe
    // ReleaseClock).
    NoiseSource quiet;
    pump (h, quiet.hop());

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    ASSERT_TRUE (snap.releaseFrozen) << "expected the live tap to freeze first";
    const double quietBefore = h.controller.quietMsForTest (0, 0);

    // The tap stops delivering data entirely -- no tap.write, no pump().
    // Advance the FakeClock past kTapSilenceTimeoutMs (250 ms) and call
    // runOnce() directly. The override still reports RISING; only tap
    // liveness may change here.
    //
    // RED IF the publish were moved back inside `if (tapAlive)`: the stale
    // `true` from the block above would persist forever, because nothing
    // would ever write `false` to a dead slot's snapshot again.
    h.clock.advance (NotchController::kTapSilenceTimeoutMs + 10.0);
    h.controller.runOnce();

    h.controller.copySnapshot (snap);
    EXPECT_FALSE (snap.releaseFrozen)
        << "a dead tap must never be reported as a frozen release clock";
    EXPECT_DOUBLE_EQ (h.controller.quietMsForTest (0, 0), quietBefore)
        << "a dead tap must not advance the quiet clock either";
}

// RED IF an INVALID ring-risk reading is treated as a freeze (spec 4.5,
// invariant 7). Detection off must release exactly as 1.1.3 did.
TEST (NotchControllerLadder, InvalidRingRiskDoesNotFreezeTheClock)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Detector));
    h.controller.setRingRiskOverrideForTest (std::make_pair (false, 1.0f));

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 31000.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -6.0);

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_FALSE (snap.releaseFrozen);
}

// RED IF a score just under the RISING band starts freezing the clock.
TEST (NotchControllerLadder, ScoreJustBelowTheRisingBandDoesNotFreeze)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0,
                                        NotchController::Origin::Detector));
    h.controller.setRingRiskOverrideForTest (
        std::make_pair (true, NotchController::kRiskFreezeFraction
                              * CandidateScorer::kConfirmScore - 0.01f));

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 31000.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -6.0);
}

// RED IF lowering the depth slider mid-show stops pulling a Detector notch up
// to the new rung (spec 4.1). The ceiling is read LIVE, every tick.
TEST (NotchControllerLadder, LoweringTheCeilingPullsADetectorNotchUpOnTheNextTick)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 200.0);
    h.controller.setNotchDefaults (30.0, -12.0);   // ceiling drops two rungs' worth
    pumpQuietFor (h, quiet, 200.0);

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -12.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -12.0);

    const Ev* ceil = nullptr;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Retune
            && e.retuneReason == NotchController::RetuneReason::Ceiling) { ceil = &e; break; }
    ASSERT_NE (ceil, nullptr);
    EXPECT_FLOAT_EQ (ceil->fromDepthDb, -18.0f);
    EXPECT_FLOAT_EQ (ceil->depthDb,     -12.0f);
}

// RED IF RAISING the ceiling deepens a notch on its own (spec 5.3). The
// ceiling branch is a one-way valve -- it pulls a notch UP to a lowered
// ceiling, and a raised one buys nothing until the bin actually rings again
// and the deepen path (Task 6) runs. Detection is OFF here precisely so
// nothing can reinforce.
TEST (NotchControllerLadder, RaisingTheCeilingDoesNotDeepenUntilTheBinRingsAgain)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Detector));

    NoiseSource quiet;
    h.controller.setNotchDefaults (30.0, -12.0);   // lower: pulls up to -12
    pumpQuietFor (h, quiet, 300.0);
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);

    h.controller.setNotchDefaults (30.0, -24.0);   // raise it all the way back
    pumpQuietFor (h, quiet, 5000.0);               // well past kDeepenAfterMs

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -12.0)
        << "raising the ceiling re-deepened a notch with no reinforce";
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -12.0);
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Retune)
            EXPECT_NE (e.retuneReason, NotchController::RetuneReason::Deepen);
}

// RED IF the slider starts dragging a Preset or Manual notch (Q8).
TEST (NotchControllerLadder, LoweringTheCeilingLeavesPresetAndManualNotchesAlone)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Preset));
    ASSERT_TRUE (h.controller.setNotch (0, 1, 1200.0, 30.0, -24.0,
                                        NotchController::Origin::Manual));

    NoiseSource quiet;
    h.controller.setNotchDefaults (30.0, -6.0);
    pumpQuietFor (h, quiet, 500.0);

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -18.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 1), -24.0);
}

// RED IF the ceiling branch fires for a non-Detector notch (B-1). This is the
// exact case that ships: tests/test_gui_wiring.cpp adopts a preset notch at
// -9.0, which is not a ladder rung. Under spec v2's quantisation the FIRST
// runOnce() after adoption would have retuned it to -6 -- a preset silently
// 3 dB shallower than the file says, and a violation of Q8 / spec 4.1. Q13
// removes the quantisation, and the Origin guard means the branch cannot come
// back even if the arithmetic changes again.
TEST (NotchControllerLadder, AnOffRungPresetDepthSurvivesTheFirstTick)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    h.controller.setNotchDefaults (30.0, -24.0);
    // The shape tests/test_gui_wiring.cpp uses: an adopted preset notch at -9.
    PresetNotch p;
    p.index = 2; p.freq = 1234.0; p.Q = 28.0; p.depthDB = -9.0;
    ASSERT_EQ (h.controller.adoptPreset ({ p }), 1);
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, 2), -9.0);

    std::vector<float> hop (512, 0.05f);
    h.tap.write (hop.data(), hop.size());
    h.controller.runOnce();

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 2), -9.0)
        << "the ceiling branch moved a Preset notch";
    for (const auto& e : r.events)
        EXPECT_NE (e.kind, Ev::Kind::Retune);
}

// RED IF a Preset notch is exempted from the RELEASE ladder, or if its reclamp
// target drifts off its own depth (spec 5.3). Q8 makes the slider unable to
// touch it; it does NOT make it immortal -- only Soundcheck is (KD-7).
TEST (NotchControllerLadder, APresetNotchReleasesDownTheLadderAndReclampsToItsOwnDepth)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    PresetNotch p;
    p.index = 0; p.freq = 1007.8125; p.Q = 30.0; p.depthDB = -12.0;
    ASSERT_EQ (h.controller.adoptPreset ({ p }), 1);

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 31000.0);   // from -12: the first rung costs 30 s
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -6.0);
    EXPECT_TRUE (h.controller.activeForTest (0, 0));

    // The howl comes back: a reclamp goes to deepestDb, which for a preset
    // notch is the depth the FILE named -- never deeper, never the slider's.
    h.controller.setDetectionActive (true);
    SineSource tone;
    for (int i = 0; i < 40; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0)
        << "a preset notch reclamped past its own depth";
}

// RED IF a reclamp waits for the 300 ms gate, or fails to return to the
// DEEPEST rung the notch ever held (spec 4.4, Q3). A howl coming back is the
// emergency this feature exists for -- it is answered on the same frame.
//
// M-A: this test lives in Task 7, not Task 6 where the branch is written,
// because the state it needs (releasedSteps > 0) can only be built by the
// RELEASE ladder.
TEST (NotchControllerLadder, AReturningHowlReclampsImmediatelyToDeepestDb)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    SineSource tone;
    for (int i = 0; i < 200; ++i)      // climb to the ceiling rung
        pump (h, tone.hop());
    ASSERT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -24.0);

    // Real release path: 30 s buys the first rung (-18), 10 s the second
    // (-12). 42 s is past both and well short of the 50 s that would take it
    // to -6 and the 60 s that would Clear it. releasedSteps is 2 here, and
    // nothing but the ladder could have set it.
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 42000.0);
    ASSERT_TRUE (h.controller.activeForTest (0, slot));
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot),   -12.0);
    ASSERT_DOUBLE_EQ (h.controller.deepestDbForTest (0, slot), -24.0)
        << "releasing must not forget the rung the room needed";
    ASSERT_EQ (h.controller.releasedStepsForTest (0, slot), 2);

    NotchCommand drained {};
    while (h.commands.read (&drained, 1) == 1) {}

    // The howl comes back. The analysis window is four hops long and tapered,
    // so a single hop of tone sitting at its very end is not yet a peak the
    // reinforce loop can see -- that is a property of the FFT window, not of
    // the reclamp, and it is why this is a short loop rather than one pump.
    // What is pinned is what the reclamp OWES: once the bin does ring, the
    // depth is back at deepestDb without waiting out the 300 ms deepen gate.
    int blocks = 0;
    while (h.controller.depthDbForTest (0, slot) > -24.0 && blocks < 8)
    {
        pump (h, tone.hop());
        ++blocks;
    }

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -24.0)
        << "the reclamp never fired";
    // Fix round 1, Minor 2: EXPECT_LT (liveMs delta, kDeepenAfterMs) was
    // tautological here -- the loop above caps at 8 blocks (~85 ms of live
    // time), which is under kDeepenAfterMs (300 ms) no matter what the
    // reclamp branch does, so it could never catch a reclamp that actually
    // waited out the deepen gate. Assert on the block count instead: the
    // analysis window is four hops long and tapered (see the comment above),
    // so a reclamp that is firing on the frame -- not waiting for the gate --
    // should land within one window's worth of hops.
    EXPECT_LE (blocks, 4)
        << "the reclamp took longer than one analysis window to fire";
    // The banked quiet time is SPENT, not merely paused. It cannot be pinned
    // at exactly 0: step 3 of the SAME runOnce adds this tick's dt back on
    // whenever the frame was not also frozen, so one block's worth is the
    // ceiling of what may legitimately be standing here.
    EXPECT_LE (h.controller.quietMsForTest (0, slot), kBlockMs);
    // After a reclamp the notch must be back on the 30 s FIRST-rung threshold.
    EXPECT_EQ (h.controller.releasedStepsForTest (0, slot), 0);

    pumpQuietFor (h, quiet, 29000.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -24.0)
        << "the reclamp left releasedSteps standing, so the notch released "
           "again after 10 s instead of 30";
    pumpQuietFor (h, quiet, 1500.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -18.0);
}

// RED IF a lowered ceiling fails to cap what a later reclamp can do (M-B).
// This is the ONE sequence that reaches the unconditional deepestDb clamp:
// the notch ends up SHALLOWER than the new ceiling, so the Set(ceiling)
// branch never runs and cannot do the clamping for it. Without the fix the
// notch reclamps to -24 under a -12 ceiling -- 12 dB louder a cut than the
// operator asked for, on a live PA (Q1, spec 4.10 invariant 2).
TEST (NotchControllerLadder, ALoweredCeilingAlsoCapsTheReclampTarget)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    ASSERT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -24.0);

    // From -24: -18 at 30 s, -12 at 40 s, -6 at 50 s, Clear at 60 s. 52 s
    // lands on -6 with 2 s of the next 10 s rung banked.
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 52000.0);
    ASSERT_TRUE (h.controller.activeForTest (0, 0));
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -6.0);
    ASSERT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -24.0);

    // The operator drops the slider two rungs while the notch sits shallow.
    // -6 is NOT deeper than -12, so the Set(ceiling) branch is skipped --
    // only the unconditional clamp can act here.
    h.controller.setNotchDefaults (30.0, -12.0);
    pumpQuietFor (h, quiet, 100.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -6.0)
        << "the ceiling branch deepened a notch that was already shallow enough";
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -12.0)
        << "the per-tick ceiling clamp skipped a notch shallower than the ceiling";

    // The howl returns.
    h.controller.setDetectionActive (true);
    SineSource tone;
    for (int i = 0; i < 40; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0)
        << "the reclamp went 12 dB past the ceiling the operator set";
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -12.0);
}

// Edge (a), found in Task 6's review and only reachable once the release
// ladder exists. RED IF the reclamp re-sends the depth already running: the
// operator sets the slider to EXACTLY the rung the notch was wound back to,
// the per-tick ceiling clamp pulls deepestDb down to it, and the reclamp
// target then equals depthDB. A Set at an unchanged depth restarts Biquad's
// 10 ms ramp on a live PA for no reason. The BOOKKEEPING must still run --
// releasedSteps back to 0, or the next release comes after 10 s not 30.
TEST (NotchControllerLadder, ACeilingLevelWithTheReleasedDepthReclampsWithoutResendingIt)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 42000.0);          // -18 at 30 s, -12 at 40 s
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);
    ASSERT_EQ (h.controller.releasedStepsForTest (0, 0), 2);

    h.controller.setNotchDefaults (30.0, -12.0);
    pumpQuietFor (h, quiet, 100.0);
    ASSERT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -12.0);
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -12.0);

    NotchCommand drained {};
    while (h.commands.read (&drained, 1) == 1) {}
    const std::size_t eventsBefore = r.events.size();

    h.controller.setDetectionActive (true);
    SineSource tone;
    for (int i = 0; i < 40; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);
    EXPECT_EQ (h.controller.releasedStepsForTest (0, 0), 0)
        << "the reclamp skipped its bookkeeping along with the redundant Set";
    for (std::size_t i = eventsBefore; i < r.events.size(); ++i)
        EXPECT_NE (r.events[i].kind, Ev::Kind::Retune)
            << "an unchanged depth was re-sent, restarting the ramp";
    while (h.commands.read (&drained, 1) == 1)
        EXPECT_FALSE (drained.type == NotchCommandType::Set
                      && drained.channel == 0 && drained.index == 0)
            << "a Set at the depth already running";
}

// Edge (b), the other half of the same review finding. RED IF a reclamp can
// emit a SHALLOWER depth under reason Reclamp. Slider raised ABOVE deepestDb
// while the notch is released: the per-tick ceiling pass (this task) must
// have already pulled the notch up to the new ceiling under reason Ceiling,
// so by the time the howl returns the reclamp target equals depthDB and
// nothing is sent. A Reclamp event reading shallower than the depth it left
// would mislabel a ceiling move as an emergency re-cut.
TEST (NotchControllerLadder, ACeilingRaisedAboveTheDeepestRungPullsUpAsCeilingNotReclamp)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));

    NoiseSource quiet;
    pumpQuietFor (h, quiet, 42000.0);          // -18 at 30 s, -12 at 40 s
    ASSERT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -12.0);

    // Slider to -6: shallower than BOTH deepestDb (-24) and the running
    // depth (-12), so the ceiling pass has to move the notch itself.
    h.controller.setNotchDefaults (30.0, -6.0);
    pumpQuietFor (h, quiet, 100.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -6.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -6.0);

    h.controller.setDetectionActive (true);
    SineSource tone;
    for (int i = 0; i < 40; ++i)
        pump (h, tone.hop());

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -6.0)
        << "the reclamp went past the ceiling the operator set";
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Retune
            && e.retuneReason == NotchController::RetuneReason::Reclamp)
            ADD_FAILURE() << "a ceiling move was labelled Reclamp, from "
                          << e.fromDepthDb << " to " << e.depthDb;
    // The move that DID happen is a Ceiling retune, -12 -> -6.
    const Ev* ceil = nullptr;
    for (const auto& e : r.events)
        if (e.kind == Ev::Kind::Retune
            && e.retuneReason == NotchController::RetuneReason::Ceiling) { ceil = &e; break; }
    ASSERT_NE (ceil, nullptr);
    EXPECT_FLOAT_EQ (ceil->fromDepthDb, -12.0f);
    EXPECT_FLOAT_EQ (ceil->depthDb,      -6.0f);
}

// RED IF a Soundcheck notch is ever released (KD-7). It already had a test
// (SoundcheckNotchNeverAutoReleases); this one pins that the LADDER does not
// touch it either -- no Retune of any reason, at any rung.
TEST (NotchControllerLadder, SoundcheckNotchesNeverRelease)
{
    Recorder r; Harness h; h.controller.setEventSink (r.sink());
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -18.0,
                                        NotchController::Origin::Soundcheck));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 60000.0);
    h.controller.runOnce();

    for (const auto& e : r.events)
        EXPECT_NE (e.kind, Ev::Kind::Retune) << "the ladder moved a soundcheck notch";
    EXPECT_TRUE (r.clears().empty());
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0), -18.0);
}

// RED IF anything in the fixture can emit a Set outside [-24, 0] (invariant
// 1). 200 randomised blocks of reinforce / quiet / frozen, watching every
// command that leaves the controller.
TEST (NotchControllerLadder, NoCommandEverLeavesTheLegalDepthRange)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    std::mt19937 rng { 20260907u };
    std::uniform_int_distribution<int> pick { 0, 2 };
    NoiseSource quiet;
    SineSource  tone;

    for (int i = 0; i < 200; ++i)
    {
        switch (pick (rng))
        {
            case 0: pump (h, tone.hop()); break;
            case 1: pump (h, quiet.hop()); break;
            case 2:
                h.controller.setRingRiskOverrideForTest (std::make_pair (true, 0.9f));
                pump (h, quiet.hop());
                h.controller.setRingRiskOverrideForTest (std::nullopt);
                break;
        }
        NotchCommand cmd {};
        while (h.commands.read (&cmd, 1) == 1)
            if (cmd.type == NotchCommandType::Set)
            {
                EXPECT_LE (cmd.depthDB,  0.0f);
                EXPECT_GE (cmd.depthDB, -24.0f);
            }
    }
}

// === Lane G, Task 8: room memory (spec 4.6, Q3/Q6/Q10, m-8, M-11) =========

// RED IF room memory stops working (spec 4.6, Q3/Q6). A howl that returns to
// the same bin within 5 minutes must not start the ladder over.
TEST (NotchControllerLadder, AHowlReturningToTheSameBinStartsAtTheRememberedDepth)
{
    Harness h;
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    int slot = -1;
    ASSERT_NO_FATAL_FAILURE (primeAndPlace (h, slot));
    ASSERT_GE (slot, 0);
    SineSource tone;
    for (int i = 0; i < 200; ++i)     // climb to the ceiling
        pump (h, tone.hop());
    const double deepest = h.controller.deepestDbForTest (0, slot);
    ASSERT_DOUBLE_EQ (deepest, -24.0);

    NoiseSource quiet;
    // From -24: 30 + 10 + 10 + 10 = 60 s to Clear. 62 s leaves 2 s of margin.
    pumpQuietFor (h, quiet, 62000.0);
    ASSERT_FALSE (h.controller.activeForTest (0, slot))
        << "the notch had not finished the release ladder, so nothing was remembered";

    // The same howl comes back. With no memory this places at -12 (SineSource
    // is a hard start, so the steep-rise branch fires); the memory must beat
    // that and go straight to what the room needed.
    SineSource again;
    int placed = -1;
    for (int i = 0; i < 60 && placed < 0; ++i)
    {
        pump (h, again.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), deepest);
}

// RED IF the bin tolerance loosens (Q10). One bin away is a DIFFERENT howl --
// at 48 kHz / 2048 that is 23.4 Hz, and an instrument partial next door must
// not inherit a -24 dB cut on its first block.
//
// m-F: the name says "starts fresh", NOT "starts at -6", because -6 is not
// what this fixture produces. SineSource switches a full-scale tone on in one
// block, so riseRatio is enormous, the steep-rise branch fires and the fresh
// placement is -12 (spec 4.3 step 2). What is being pinned here is that the
// depth came from the PLACEMENT POLICY and not from room memory, so the
// assertion is exact: -12, never -24, and never a loose >= -12, which -24
// would also have to fail but which would silently accept -6 if the
// steep-rise branch broke.
TEST (NotchControllerLadder, OneBinAwayIsANewHowlAndStartsFreshNotFromMemory)
{
    Harness h;
    const double binHz = kTestSr / Detector::kFftSize;   // 23.4375
    h.controller.setNotchDefaults (30.0, -24.0);

    // Place at the ceiling by hand, then let the ladder clear it: the memory
    // entry is written by the AutoRelease at the bottom of the ladder.
    ASSERT_TRUE (h.controller.setNotch (0, 0, 43.0 * binHz, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    // A candidate exactly ONE bin up. Memory must not answer for it.
    h.controller.setDetectionActive (true);

    // processSpectrumForDetection returns early while detection is off, so the
    // quiet pump above left the scorer with NO history. Warm it the same way
    // primeAndPlace does before the tone arrives, or nothing ever confirms.
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    SineSource neighbour; neighbour.freq = 44.0 * binHz;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, neighbour.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -12.0)
        << "a neighbouring bin inherited the remembered depth (or the "
           "steep-rise placement branch stopped firing)";
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, placed), -12.0)
        << "the memory -24 leaked into deepestDb even though the depth was "
           "placed fresh -- the next reclamp would then go to -24";
}

// What this pins: after kMemoryTtlMs of silence the placement comes out of the
// PLACEMENT POLICY and nowhere else -- exactly -12, the steep-rise rung a
// hard-onset SineSource earns on its own (spec 4.3 step 2). 5 minutes and one
// millisecond is a different show.
//
// It is a NEGATIVE control: "memory did NOT fire". It therefore also passes
// with the whole feature deleted, and it is not evidence that room memory
// works -- AHowlReturningToTheSameBinStartsAtTheRememberedDepth is. What the
// exact -12 buys over the `>= -12` this used to assert is that a TTL bug which
// returned some OTHER remembered depth (a -6 entry, say, under a read path
// that still assigned instead of deepening) can no longer slip through.
TEST (NotchControllerLadder, RoomMemoryExpiresAfterFiveMinutes)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);                       // -24: cleared, remembered
    ASSERT_FALSE (h.controller.activeForTest (0, 0));
    pumpQuietFor (h, quiet, NotchController::kMemoryTtlMs + 1000.0);   // expired

    h.controller.setDetectionActive (true);

    // processSpectrumForDetection returns early while detection is off, so the
    // quiet pump above left the scorer with NO history. Warm it the same way
    // primeAndPlace does before the tone arrives, or nothing ever confirms.
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    SineSource tone;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, tone.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -12.0)
        << "an expired entry was still used";
}

// RED IF a remembered entry can be used twice (spec 4.6). It describes ONE
// release; a second howl at that bin has to earn its own depth.
TEST (NotchControllerLadder, ARememberedEntryIsUsedOnlyOnce)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    h.controller.setDetectionActive (true);

    // processSpectrumForDetection returns early while detection is off, so the
    // quiet pump above left the scorer with NO history. Warm it the same way
    // primeAndPlace does before the tone arrives, or nothing ever confirms.
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    SineSource tone;
    int first = -1;
    for (int i = 0; i < 80 && first < 0; ++i)
    {
        pump (h, tone.hop());
        first = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (first, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, first), -24.0);

    // Clear it by hand (no AutoRelease => no new memory entry) and let it
    // place again: the ladder must start from scratch.
    h.controller.clearNotch (0, first, NotchController::ClearReason::Manual);
    ASSERT_FALSE (h.controller.activeForTest (0, first));
    int second = -1;
    for (int i = 0; i < 80 && second < 0; ++i)
    {
        pump (h, tone.hop());
        second = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (second, 0);
    // The second half is a NEGATIVE control: what it pins is that the memory
    // did NOT fire, so the depth is the placement policy's own -12 and not the
    // -24 of the first half. Asserted exactly, because `>= -12` would also
    // have accepted -6 -- i.e. it would have passed with the steep-rise branch
    // broken, and it passed with room memory deleted entirely.
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, second), -12.0)
        << "the entry was consumed twice";
}

// RED IF the ceiling stops capping a remembered depth (spec 4.3 step 4, m-8).
// A -24 memory under a -12 ceiling must place at -12. Under Q13 that is the
// ONLY thing the read path does to a remembered depth: no rung quantisation,
// because the ceiling IS the deepest legal rung. A memory holding an odd depth
// (a Manual -9 that auto-released) is therefore placed at -9 verbatim under a
// -24 ceiling -- correct, and covered by the test after this one.
TEST (NotchControllerLadder, ARememberedDepthIsStillCappedByTheCeiling)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    h.controller.setNotchDefaults (30.0, -12.0);   // ceiling drops
    h.controller.setDetectionActive (true);

    // processSpectrumForDetection returns early while detection is off, so the
    // quiet pump above left the scorer with NO history. Warm it the same way
    // primeAndPlace does before the tone arrives, or nothing ever confirms.
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    SineSource tone;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, tone.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -12.0);
}

// RED IF the read path snaps a remembered depth to a fixed rung (m-8). A
// Manual notch is placed at whatever the operator typed, so an entry can hold
// -15; under a ceiling that permits it, -15 is what comes back. Quantising to
// -12 would throw away 3 dB the room demonstrably needed, and quantising to
// -18 would place DEEPER than the notch ever ran, which invariant 2 forbids.
//
// The value is -15 and not the -9 this test used before the Q14 ruling: under
// Q14 the memory may only DEEPEN (step 3 is a min against the -12 the
// hard-onset SineSource places on its own), so a -9 entry is correctly
// ignored here and could no longer say anything about rung snapping. -15 sits
// off-rung on the deep side of -12, where the memory does decide the depth.
TEST (NotchControllerLadder, AnOffRungRememberedDepthComesBackVerbatim)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -15.0,
                                        NotchController::Origin::Manual));
    NoiseSource quiet;
    // From -15: -12 at 30 s, -6 at 40 s, Clear at 50 s. 52 s leaves margin.
    // deepestDb stays -15 throughout -- releasing gives depth back, it does
    // not rewrite what the notch once needed -- so -15 is what is remembered.
    pumpQuietFor (h, quiet, 52000.0);
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    h.controller.setDetectionActive (true);

    // processSpectrumForDetection returns early while detection is off, so the
    // quiet pump above left the scorer with NO history. Warm it the same way
    // primeAndPlace does before the tone arrives, or nothing ever confirms.
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    SineSource tone;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, tone.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -15.0);
}

// What this pins: after any of the three room-change boundaries, the next
// placement is exactly -12 -- the steep-rise rung the placement policy earns
// on its own -- so nothing of the pre-change room survived into it. setWidth
// is the boundary that actually fires in the shipping app: MainComponent's
// onAfterRestart hook calls it on every engine restart, i.e. every device,
// rate and buffer change.
//
// Another NEGATIVE control: it says "memory did NOT fire", which is also true
// with the feature deleted. The exact -12 (this asserted `>= -12` before) is
// what stops a wipe bug that leaves a SHALLOWER entry behind from passing.
TEST (NotchControllerLadder, SetWidthClearAllAndSetSampleRateWipeRoomMemory)
{
    for (int which = 0; which < 3; ++which)
    {
        Harness h;
        h.controller.setNotchDefaults (30.0, -24.0);
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                            NotchController::Origin::Detector));
        NoiseSource quiet;
        pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
        ASSERT_FALSE (h.controller.activeForTest (0, 0)) << "which=" << which;

        if (which == 0) h.controller.setWidth (2);
        if (which == 1) h.controller.clearAll();
        if (which == 2) h.controller.setSampleRate (48000.0);

        h.controller.setDetectionActive (true);

        // processSpectrumForDetection returns early while detection is off, so the
        // quiet pump above left the scorer with NO history. Warm it the same way
        // primeAndPlace does before the tone arrives, or nothing ever confirms.
        for (int i = 0; i < kWarmupBlocks; ++i)
            pump (h, quiet.hop());
        SineSource tone;
        int placed = -1;
        for (int i = 0; i < 80 && placed < 0; ++i)
        {
            pump (h, tone.hop());
            placed = firstActiveIndex (h.controller, 0);
        }
        ASSERT_GE (placed, 0) << "which=" << which;
        EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -12.0)
            << "memory survived a room change, which=" << which;
    }
}

// RED IF a LINKED release leaves an orphan entry on one lane (M-11). One
// Clear writes both lanes; one placement consumes both.
TEST (NotchControllerLadder, LinkedReleaseWritesAndConsumesBothLanes)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    ASSERT_TRUE (h.controller.setNotch (1, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));

    NoiseSource quietL, quietR; quietR.rng.seed (999u);
    // From -24: 30 + 10 + 10 + 10 = 60 s to Clear, +2 s margin.
    const int blocks = (int) std::lround (62000.0 / kBlockMs);
    for (int i = 0; i < blocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());
    ASSERT_FALSE (h.controller.activeForTest (0, 0));
    ASSERT_FALSE (h.controller.activeForTest (1, 0));

    h.controller.setDetectionActive (true);

    // processSpectrumForDetection returns early while detection is off, so the
    // quiet pump above left the scorer with NO history. Warm it the same way
    // primeAndPlace does before the tone arrives, or nothing ever confirms.
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());
    SineSource toneL, toneR;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pumpStereo (h, toneL.hop(), toneR.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -24.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (1, placed), -24.0);
}

// RED IF room memory is allowed to make a placement SHALLOWER (owner ruling
// Q14, docs/superpowers/decisions/2026-09-06-lane-g-gain-aware-notch.md; spec
// 4.3 step 3). An entry records what a bin needed LAST time -- and "last time"
// includes a notch that never had to dig at all. A -6 that never reinforced,
// or an operator's Manual -3 left to age out, would otherwise CLAMP the next
// placement at that bin to -3 and undercut the -12 the steep-rise branch just
// asked for, on a howl that is by then loud enough to have triggered it. The
// memory may only ever deepen.
TEST (NotchControllerLadder, RoomMemoryNeverShallowsAPlacement)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    // -3: shallower than either placement rung. Manual notches carry their own
    // ceiling (Q8) and never reclamp, so -3 is exactly what deepestDb holds
    // and exactly what the AutoRelease at the bottom of the ladder remembers.
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -3.0,
                                        NotchController::Origin::Manual));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 32000.0);   // -3 is already at/above -6: Clear at 30 s
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    h.controller.setDetectionActive (true);

    // processSpectrumForDetection returns early while detection is off, so the
    // quiet pump above left the scorer with NO history. Warm it the same way
    // primeAndPlace does before the tone arrives, or nothing ever confirms.
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    SineSource tone;
    int placed = -1;
    for (int i = 0; i < 80 && placed < 0; ++i)
    {
        pump (h, tone.hop());
        placed = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (placed, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, placed), -12.0)
        << "the remembered -3 clamped the steep-rise placement shallower";
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, placed), -12.0)
        << "a shallower remembered depth leaked into deepestDb";
}

// RED IF a Soundcheck placement SPENDS a room-memory entry (M-1). Its depth is
// the slider and nothing else (KD-7, the override at the end of the depth
// choice), so a lookup at that bin can only throw the room's history away --
// and the DETECTOR howl that comes back to the same bin afterwards is then
// left crawling up from -6 with nothing to show for it.
TEST (NotchControllerLadder, ASoundcheckPlacementDoesNotSpendTheRoomMemory)
{
    Harness h;
    h.controller.setNotchDefaults (30.0, -24.0);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1007.8125, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    NoiseSource quiet;
    pumpQuietFor (h, quiet, 62000.0);   // from -24: 60 s to Clear, +2 s margin
    ASSERT_FALSE (h.controller.activeForTest (0, 0));

    // Soundcheck arms detection for 15 s; its notch lands at the slider.
    h.controller.startSoundcheck();
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    SineSource tone;
    int sc = -1;
    for (int i = 0; i < 80 && sc < 0; ++i)
    {
        pump (h, tone.hop());
        sc = firstActiveIndex (h.controller, 0);
    }
    ASSERT_GE (sc, 0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, sc), -24.0)   // KD-7
        << "the soundcheck notch was not placed at the slider";

    // Take it away by hand (Manual => no new entry), let soundcheck expire,
    // and let the DETECTOR place at the same bin. The -24 must still be there.
    // Both lanes: the placement was LINKED (a mono Harness has no lane-1 tap,
    // so effectiveLinked() is true), and a notch left standing on lane 1 is a
    // locked fundamental the harmonic penalty would hold against the probe.
    for (int lane = 0; lane < NotchController::kChannels; ++lane)
        h.controller.clearNotch (lane, sc, NotchController::ClearReason::Manual);
    pumpQuietFor (h, quiet, 16000.0);   // 15 s of soundcheck, +1 s
    ASSERT_FALSE (h.controller.soundcheckActive());

    h.controller.setDetectionActive (true);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());
    EXPECT_DOUBLE_EQ (probeMemoryAt (h, quiet, 1000.0), -24.0)
        << "the soundcheck placement consumed the entry, so the detector had "
           "to start over at the steep-rise rung";
}

// RED IF the ring lets a consumed slot rot (M-2). kMemoryEntriesPerLane is a
// promise about how many bins a room may be remembered at; a slot whose entry
// has been used has to go back into service, or every consume permanently
// shrinks the ring and the head evicts a LIVE neighbour instead.
//
// Both halves of the ring's contract are pinned here, in one fixture because
// each half needs the other's setup:
//   phase A -- 16 entries then a 17th, with nothing free: the OLDEST write is
//              the one that goes;
//   phase B -- one entry consumed, then an 18th write: it must land in that
//              hole and NOT on the head, which is pointing at a live entry.
TEST (NotchControllerLadder, TheMemoryRingReclaimsAConsumedSlotBeforeEvictingALiveOne)
{
    Harness h;
    const double binHz = kTestSr / Detector::kFftSize;   // 23.4375
    h.controller.setNotchDefaults (30.0, -24.0);
    auto drainCommands = [&] { NotchCommand cmd {}; while (h.commands.read (&cmd, 1) == 1) {} };

    // 16 Manual notches at 16 distinct bins. -18 because a memory hit has to
    // be distinguishable from the -12 a hard-onset SineSource places on its
    // own; Manual because those carry their own ceiling (Q8) and so release
    // from exactly -18 without the live slider deepening them first. They all
    // clear on the same tick, in index order, so slot k of the ring holds
    // bin 40 + k and the head is back at 0.
    NoiseSource quiet;
    for (int i = 0; i < NotchController::kSlots; ++i)
        ASSERT_TRUE (h.controller.setNotch (0, i, (40.0 + i) * binHz, 30.0, -18.0,
                                            NotchController::Origin::Manual)) << "i=" << i;
    pumpQuietFor (h, quiet, 52000.0);   // -18: -12 at 30 s, -6 at 40 s, Clear at 50 s
    for (int i = 0; i < NotchController::kSlots; ++i)
        ASSERT_FALSE (h.controller.activeForTest (0, i)) << "i=" << i;
    drainCommands();

    // --- phase A: a 17th write with every slot LIVE evicts the oldest ------
    ASSERT_TRUE (h.controller.setNotch (0, 0, 60.0 * binHz, 30.0, -18.0,
                                        NotchController::Origin::Manual));
    pumpQuietFor (h, quiet, 52000.0);
    ASSERT_FALSE (h.controller.activeForTest (0, 0));
    drainCommands();

    h.controller.setDetectionActive (true);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    EXPECT_DOUBLE_EQ (probeMemoryAt (h, quiet, 40.0 * binHz), -12.0)
        << "bin 40 was the oldest write and had to be the one the 17th evicted";
    // Bin 42 is alive, and probing it CONSUMES it -- that is the hole phase B
    // needs. Deliberately not bin 41: the head is on bin 41 after the eviction
    // above, and a hole there would be indistinguishable from the head.
    EXPECT_DOUBLE_EQ (probeMemoryAt (h, quiet, 42.0 * binHz), -18.0)
        << "the 17th write evicted more than the single oldest entry";

    // --- phase B: an 18th write must refill the hole, not eat the head -----
    h.controller.setDetectionActive (false);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 61.0 * binHz, 30.0, -18.0,
                                        NotchController::Origin::Manual));
    pumpQuietFor (h, quiet, 52000.0);
    ASSERT_FALSE (h.controller.activeForTest (0, 0));
    drainCommands();

    h.controller.setDetectionActive (true);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pump (h, quiet.hop());

    EXPECT_DOUBLE_EQ (probeMemoryAt (h, quiet, 41.0 * binHz), -18.0)
        << "the head evicted a LIVE entry while a consumed slot sat empty";
    EXPECT_DOUBLE_EQ (probeMemoryAt (h, quiet, 61.0 * binHz), -18.0)
        << "the 18th write did not land anywhere findable";
}
