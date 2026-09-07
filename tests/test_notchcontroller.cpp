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

TEST (NotchControllerAutoRelease, LiveTapReleasesAfter30s)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    std::vector<float> hop (512, 0.1f);
    for (int i = 0; i < 7000; ++i) {          // 7000 * 5 ms = 35 s of live audio
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
    for (int i = 0; i < 7000; ++i) {          // 35 s of live audio
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
    // Runtime defaults (brief 2026-08-24): Q 30, depth -18.
    EXPECT_FLOAT_EQ (cmd.Q, 30.0f);
    EXPECT_FLOAT_EQ (cmd.depthDB, -18.0f);
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

TEST (NotchControllerDetection, HowlThatStopsAutoReleasesAfter30s)
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
    // feeds peakiness at the notch bin, so the 30 s release timer runs out.
    NoiseSource quiet;
    for (int i = 0; i < 3500; ++i)   // 3500 * 10.667 ms ~= 37.3 s
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
    EXPECT_FLOAT_EQ (cmd.depthDB, -24.0f);
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
    const int blocks = (int) (NotchController::kAutoReleaseMs / kBlockMs) + 20;
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
    const int blocks = (int) (NotchController::kAutoReleaseMs / kBlockMs) + 20;
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
    // AutoRelease: mirror LiveTapReleasesAfter30s (this file, ~line 151).
    {
        Recorder r; Harness h; h.controller.setEventSink (r.sink());
        ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
        NoiseSource quiet;
        const int blocks = (int) (NotchController::kAutoReleaseMs / kBlockMs) + 10;
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
