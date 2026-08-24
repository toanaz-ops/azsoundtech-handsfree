// tests/test_notchcontroller.cpp
#include <gtest/gtest.h>
#include "app/NotchController.h"
#include "dsp/PeakinessAnalyzer.h"

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
    double freq       = 1000.0;   // bin 21 = 984.375 Hz at 48 kHz
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
        if (PeakinessAnalyzer::peakinessAt (snap.magnitudes.data(),
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
    // KD-5: fixed automatic params.
    EXPECT_FLOAT_EQ (cmd.Q, 30.0f);
    EXPECT_FLOAT_EQ (cmd.depthDB, -12.0f);
    // Frequency within half a bin (23.44 Hz) of the 1 kHz tone.
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
