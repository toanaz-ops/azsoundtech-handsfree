// tests/test_soundcheckcontroller.cpp
//
// Fake clock, real AudioEngine (no device -- the callback is public and driven
// by hand, exactly as tests/test_audioengine.cpp does it), no thread: every test
// calls runOnce() itself.
#include <gtest/gtest.h>

#include "app/AudioEngine.h"
#include "app/NotchController.h"
#include "app/SoundcheckController.h"

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;

// std::atomic because AbortAndJoinJoinsBeforeItRuns advances it from the test
// thread while the REAL poll thread reads it; a plain double there is a data
// race, and a race in the fixture is a flaky test nobody can explain later.
class FakeClock : public ClockSource
{
public:
    double nowMs() const override { return ms_.load (std::memory_order_relaxed); }
    void advance (double m) { ms_.store (ms_.load (std::memory_order_relaxed) + m,
                                         std::memory_order_relaxed); }
private:
    std::atomic<double> ms_ { 1000.0 };
};

// Declared before Rig so Rig::armWith can default to it.
NotchController::SnapshotBuffer risk (bool valid, float score)
{
    NotchController::SnapshotBuffer s {};
    s.ringRiskValid     = valid;
    s.ringRiskScore     = score;
    s.ringRiskThreshold = 0.7f;      // what NotchController.cpp:648 publishes
    return s;
}

// Drives `ms` of audio through the engine while polling the controller, so the
// sample index and the wall clock stay consistent. 256-sample blocks at 48 kHz
// are 5.33 ms, i.e. about one poll each.
struct Rig
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    FakeClock   clock;
    SoundcheckController sc { engine, clock };

    std::vector<std::vector<float>> in, out;
    std::vector<const float*> inPtr;
    std::vector<float*>       outPtr;
    int channels = 2, frames = 256;
    std::vector<float> micSource;        // what the "room" feeds back, looped
    std::size_t micPos = 0;
    int detectionCalls = 0; bool detectionOn = true;
    // I-1. Sampling detectionOn after runOnce() returns cannot see the ORDER of
    // two statements inside finishRun(); swapping them leaves that assertion
    // green. What inv 12 actually forbids is detection being restored once
    // Results is already published, so the restore itself records the state it
    // ran in.
    bool detectionEverRestored = false;
    SoundcheckController::State stateWhenDetectionRestored
        = SoundcheckController::State::Idle;
    std::vector<juce::var> log;

    explicit Rig (int chans = 2) : channels (chans)
    {
        in.assign  ((std::size_t) channels, std::vector<float> ((std::size_t) frames, 0.0f));
        out.assign ((std::size_t) channels, std::vector<float> ((std::size_t) frames, 0.0f));
        for (auto& v : in)  inPtr.push_back (v.data());
        for (auto& v : out) outPtr.push_back (v.data());

        sc.setDetectionActiveOnAllSlots = [this] (bool on)
        {
            detectionOn = on;
            ++detectionCalls;
            if (on)
            {
                detectionEverRestored     = true;
                stateWhenDetectionRestored = sc.getState();
            }
        };
        sc.logEvent = [this] (const juce::var& v) { log.push_back (v); };

        SlotConfig c; c.enabled = true; c.width = 2;
        c.inputChannels[0] = 0; c.inputChannels[1] = 1;
        c.outputChannels[0] = 0; c.outputChannels[1] = 1;
        engine.setSlotConfig (0, c);

        // B-4, and it takes BOTH halves.
        //
        // isRunning_ is set only by start() (AudioEngine.cpp:86);
        // audioDeviceAboutToStart() does not touch it, so plan rev 1's
        // `engine.audioDeviceAboutToStart (nullptr)` left preflight() returning
        // EngineNotRunning and every armed run aborting engine_stopped.
        setRunning (true);
        // numInputChannels_/numOutputChannels_ are written ONLY from the
        // callback (:635-636), so preflight's channel-count check needs one
        // real block to have flowed first.
        block();
    }

    // Split out so RefusesWhenEngineNotRunning can leave it false.
    void setRunning (bool running) { engine.setRunningForTest (running); }

    // arm() takes its own risk snapshot (S-1). Most fixtures do not care what
    // it says, so they get the "nothing scored yet" snapshot -- the state the
    // app is in every time it opens.
    SoundcheckController::Refusal
    armWith (const SoundcheckController::RunParams& p,
             const NotchController::SnapshotBuffer& snap = risk (false, 0.0f))
    {
        return sc.arm (stereoTargets(), p, snap);
    }

    // Drives blocks and the clock WITHOUT polling -- the device keeps running
    // while the lane M thread is asleep.
    void driveUnpolled (double ms)
    {
        const double perBlock = frames / kSr * 1000.0;
        for (double t = 0.0; t < ms; t += perBlock)
        {
            block();
            clock.advance (perBlock);
        }
    }

    void block()
    {
        // Feed the mic channels from micSource so the noise-floor gate and the
        // hot-mic abort see something real.
        for (int ch = 0; ch < channels; ++ch)
            for (int n = 0; n < frames; ++n)
                in[(std::size_t) ch][(std::size_t) n] =
                    micSource.empty() ? 0.0f
                                      : micSource[(micPos + (std::size_t) n) % micSource.size()];
        micPos += (std::size_t) frames;

        for (auto& v : out) std::fill (v.begin(), v.end(), 0.0f);
        const juce::AudioIODeviceCallbackContext ctx {};
        engine.audioDeviceIOCallbackWithContext (inPtr.data(), channels,
                                                 outPtr.data(), channels, frames, ctx);
    }

    void pump (double ms)
    {
        const double perBlock = frames / kSr * 1000.0;
        for (double t = 0.0; t < ms; t += perBlock)
        {
            block();
            clock.advance (perBlock);
            sc.runOnce();
        }
    }

    // Pumps until `s` is reached, or `limitMs` of wall time elapses. Needed
    // because a Results deadline has to be measured FROM the moment Results is
    // entered: pumping a fixed 12000 ms first spends ~3 s of the 20 s window
    // before the measurement even starts (I-1, and rev 1's own arithmetic ran
    // past the timeout it was asserting).
    bool pumpUntil (SoundcheckController::State s, double limitMs)
    {
        const double perBlock = frames / kSr * 1000.0;
        for (double t = 0.0; t < limitMs; t += perBlock)
        {
            if (sc.getState() == s) return true;
            block();
            clock.advance (perBlock);
            sc.runOnce();
        }
        return sc.getState() == s;
    }

    float peakOn (int ch) const
    {
        float m = 0.0f;
        for (float v : out[(std::size_t) ch]) m = std::max (m, std::abs (v));
        return m;
    }

    std::vector<SoundcheckController::Target> stereoTargets() const
    {
        return { { 0, 0, 0, 0 }, { 0, 1, 1, 1 } };
    }

    SoundcheckController::RunParams params (float gate = 10.0f) const
    {
        SoundcheckController::RunParams p;
        p.noiseFloorGate    = gate;
        p.peak              = SoundcheckSignal::kSoundcheckMaxPeak;
        p.sampleRate        = kSr;
        p.numInputChannels  = channels;
        p.numOutputChannels = channels;
        p.ceilingDb         = -24.0;
        p.notchQ            = 30.0;
        return p;
    }
};

std::vector<float> whiteNoise (std::size_t n, float sigma, unsigned seed = 3)
{
    std::mt19937 rng { seed };
    std::normal_distribution<float> d { 0.0f, sigma };
    std::vector<float> v (n);
    for (auto& x : v) x = d (rng);
    return v;
}

std::vector<float> noisePlusTone (std::size_t n, float sigma, double hz, float amp)
{
    auto v = whiteNoise (n, sigma);
    for (std::size_t i = 0; i < n; ++i)
        v[i] += amp * (float) std::sin (2.0 * 3.14159265358979323846 * hz * (double) i / kSr);
    return v;
}

// B-10: returns juce::String BY VALUE. Rev 1 returned const char* from
// toRawUTF8() of a temporary juce::String -- the temporary dies at the end of
// the full expression and every caller read freed memory.
juce::String evOf (const juce::var& v)
{
    auto* o = v.getDynamicObject();
    return o == nullptr ? juce::String() : o->getProperty ("ev").toString();
}

bool sawEvent (const std::vector<juce::var>& log, const char* name)
{
    for (const auto& v : log)
        if (evOf (v) == name) return true;
    return false;
}

// S-2/I-3. EVERY abort test asserts the same thing about the ENGINE, because
// "the log said it aborted" is a statement about the controller and says
// nothing about whether a PA is still being driven.
//
// The blocks are load-bearing: an abort while the sweep is audible hands the
// fade to the CALLBACK (inv 9), so the channel is released a ramp later, not
// at the instant of the abort. Driving them is what lets this assert the end
// state rather than the intent.
void expectEngineStoodDown (Rig& r)
{
    for (int i = 0; i < 16; ++i) r.block();      // let any fade finish

    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), -1) << "still armed";
    EXPECT_FALSE (r.engine.soundcheckIsEmitting());
    EXPECT_FALSE (r.engine.isSoundcheckRampOutPending()) << "a fade left pending";

    // The capture gate is shut: driving the device adds nothing to the ring.
    auto& mic = r.engine.getMicCaptureBuffer();
    std::vector<float> sink (mic.getAvailableRead() + 1u, 0.0f);
    while (mic.read (sink.data(), sink.size()) > 0) {}
    r.block();
    EXPECT_EQ (r.engine.getMicCaptureBuffer().getAvailableRead(), 0u)
        << "capture is still active after the abort";

    // ...and the taps are live again, which is the half that can actually hurt:
    // a suspended tap is a deaf feedback killer.
    auto& tap = r.engine.getTapBuffer (0, 0);
    std::vector<float> drain (8192, 0.0f);
    while (tap.read (drain.data(), drain.size()) > 0) {}
    r.block();
    EXPECT_GT (tap.getAvailableRead(), 0u) << "the taps are still suspended";
}

juce::String abortReasonInLog (const std::vector<juce::var>& log)
{
    for (const auto& v : log)
        if (evOf (v) == "soundcheck_abort")
            return v.getDynamicObject()->getProperty ("reason").toString();
    return {};
}

// ======================= TASK 7 HELPERS: applySoundcheckResults =============
//
// B-2: tests/test_notchcontroller.cpp's Recorder lives in THAT translation
// unit's anonymous namespace and is invisible here, it has no operator() (the
// sink comes from sink()), and it must outlive the controller it is wired to,
// because the controller's destructor flushes through the sink.
//
// N-1: and that is not only about where the two TYPES are defined. In EVERY
// TEST BODY that wires a recorder to a rig, the `EventRecorder` OBJECT must be
// declared before the `NotchRig` OBJECT. Locals are destroyed in reverse
// declaration order, so `NotchRig rig; EventRecorder rec;` puts the flush from
// ~NotchController into a vector that has already gone. The bug is silent in a
// passing run and shows up as a heap corruption somewhere else.
//
// It is not promoted into tests/test_gui_helpers.h: that header is GUI-only
// (namespace gui_test, and its only include is juce_gui_basics), so putting a
// NotchController::NotchEvent recorder in it would drag app/NotchController.h
// into every GUI test TU.
struct EventRecorder
{
    std::vector<NotchController::NotchEvent> events;

    NotchController::EventSink sink()
    {
        return [this] (const NotchController::NotchEvent& e) { events.push_back (e); };
    }

    bool sawClearWithReason (NotchController::ClearReason r) const
    {
        for (const auto& e : events)
            if (e.kind == NotchController::NotchEvent::Kind::Clear && e.reason == r)
                return true;
        return false;
    }
};

// A NotchController wired the way MainComponent wires one, with the width and
// the lane-1 tap set EXPLICITLY. Lane G lesson 18: a mono harness left at
// width_ == 2 turns effectiveLinked() on and writes a dead lane-1 entry that
// poisons the NEXT test in the same file. Never rely on the default.
//
// B-1(c): pump() exists because latest_ is published only inside runOnce()'s
// drain loop, and that loop body runs only when a block was actually drained
// off a tap (NotchController.cpp:550-560). Without it copySnapshot() returns an
// empty buffer forever, every snapshot-reading assertion passes vacuously, and
// laneCount keeps its default of 1 (NotchController.h:412). Precedent:
// tests/test_gui_wiring.cpp:1033-1035.
struct NotchRig
{
    LockFreeRingBuffer<float> tapL { 8192 }, tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> cmds { 128 };
    FakeClock clock;
    int lanes = 1;
    std::unique_ptr<NotchController> controller;

    explicit NotchRig (int laneCount) : lanes (laneCount)
    {
        if (lanes == 2) controller = std::make_unique<NotchController> (tapL, &tapR, cmds, clock);
        else            controller = std::make_unique<NotchController> (tapL, cmds, clock);
        controller->setWidth (lanes);
        pump();                       // so the FIRST snapshot is a real one
    }

    void pump()
    {
        const std::vector<float> block (512, 0.0f);   // exactly one Detector hop
        tapL.write (block.data(), block.size());
        if (lanes == 2)
            tapR.write (block.data(), block.size());  // B-1(b): laneCount needs BOTH
        controller->runOnce();
    }

    NotchController::SnapshotBuffer snapshot()
    {
        pump();
        NotchController::SnapshotBuffer s {};
        controller->copySnapshot (s);
        return s;
    }

    int countSoundcheckNotches()
    {
        const auto snap = snapshot();
        int n = 0;
        for (std::uint32_t i = 0; i < snap.notchCount; ++i)
            if (snap.notches[i].origin == NotchController::Origin::Soundcheck)
                ++n;
        return n;
    }
};

SoundcheckController::OutputResult oneCandidate (int slot, int lane, double hz, double depthDb)
{
    SoundcheckController::OutputResult r;
    r.slot = slot; r.lane = lane; r.measured = true;
    r.candidateCount = 1;
    r.candidates[0].hz       = (float) hz;
    r.candidates[0].q        = 30.0f;
    r.candidates[0].depthDb  = (float) depthDb;
    r.candidates[0].marginDb = -9.0f;
    return r;
}
} // namespace

// ------------------------------------------------------------- REFUSALS ----
//
// Each asserts that NOT ONE SAMPLE was emitted and the state is back to Idle
// (inv 19).

// RED IF: Preflight stops checking isRunning(). AudioEngine's channel counts are
// written only from the callback and are never reset on stop, so a non-zero
// count can be a leftover from the previous device session -- checking the
// counts alone is not enough. F26.
TEST (SoundcheckController, RefusesWhenEngineNotRunning)
{
    Rig r;
    r.setRunning (false);                     // B-4: the Rig ctor turned it on
    EXPECT_EQ (r.sc.preflight (r.stereoTargets(), risk (false, 0.0f)),
               SoundcheckController::Refusal::EngineNotRunning);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    r.pump (100.0);
    EXPECT_EQ (r.peakOn (0), 0.0f);
    EXPECT_EQ (r.peakOn (1), 0.0f);
}

// RED IF: a zero-channel device is treated as measurable.
TEST (SoundcheckController, RefusesWithZeroChannels)
{
    Rig r;
    // m-16: preflight() takes targets and a risk snapshot -- it does NOT take
    // RunParams. An empty target list is what "nothing to measure" looks like.
    EXPECT_EQ (r.sc.preflight ({}, risk (false, 0.0f)),
               SoundcheckController::Refusal::NoChannels);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: a disabled slot is measured anyway. Its lanes are not in the callback's
// lane table at all, so the sweep would go out with nothing to compare it
// against.
TEST (SoundcheckController, RefusesWhenSlotDisabled)
{
    Rig r;
    SlotConfig c = r.engine.getSlotConfig (0); c.enabled = false;
    r.engine.setSlotConfig (0, c);

    EXPECT_EQ (r.sc.preflight (r.stereoTargets(), risk (false, 0.0f)),
               SoundcheckController::Refusal::SlotDisabled);
    r.pump (100.0);
    EXPECT_EQ (r.peakOn (0), 0.0f);
}

// RED IF: an invalid channel pair is allowed to degrade into "could not
// measure". The lane loop merely `continue`s past a bad pair, so without this
// check bad routing reads as a quiet room instead of as a routing fault. F26.
TEST (SoundcheckController, RefusesOnInvalidChannelPair)
{
    Rig r;
    std::vector<SoundcheckController::Target> bad { { 0, 0, 99, 0 } };

    EXPECT_EQ (r.sc.preflight (bad, risk (false, 0.0f)),
               SoundcheckController::Refusal::InvalidChannelPair);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    r.pump (100.0);
    EXPECT_EQ (r.peakOn (0), 0.0f);
}

// RED IF: the RISING identity is written as score >= threshold (which would
// refuse only above 0.7, not 0.385), or -- far more likely -- if
// ringRiskValid == false is treated as a refusal. The third case is the one
// that gets implemented wrong: "not scored yet" is the state the app is in every
// time it opens, and refusing there locks out the commonest use of the feature.
// R3-2.
TEST (SoundcheckController, RefusesWhenRingRiskIsRising)
{
    Rig r;
    const auto targets = r.stereoTargets();

    // 0.55 * 0.7 = 0.385.
    EXPECT_EQ (r.sc.preflight (targets, risk (true, 0.4f)),
               SoundcheckController::Refusal::RingRiskRising);
    EXPECT_EQ (r.sc.preflight (targets, risk (true, 0.3f)),
               SoundcheckController::Refusal::None);
    EXPECT_EQ (r.sc.preflight (targets, risk (false, 0.9f)),
               SoundcheckController::Refusal::None);

    // ...and the third case is RECORDED, not silently dropped. The snapshot the
    // LAST preflight saw is what soundcheck_start carries: arm() takes no
    // snapshot of its own (the message thread re-runs preflight immediately
    // before arming), so this is the only route the number has into the log.
    ASSERT_EQ (r.sc.arm (targets, r.params(), risk (false, 0.9f)),
               SoundcheckController::Refusal::None);
    r.pump (20.0);
    bool sawNullRisk = false;
    for (const auto& v : r.log)
        if (evOf (v) == "soundcheck_start")
            sawNullRisk = v.getDynamicObject()->getProperty ("ring_risk").isVoid();
    EXPECT_TRUE (sawNullRisk) << "ring_risk must be null, not 0.0";
}

// RED IF: arm() lets a caller bug through. A non-finite ceiling reaches
// SoundcheckCandidates as "unset" and yields marks with NO proposals, and a
// zero peakiness gate aborts every run in every room -- both would look like
// data about the ROOM. Neither may reach a loudspeaker or an operator. Task 3
// I-3, N1.
TEST (SoundcheckController, ArmRefusesAnUnsetCeilingOrAnUnsetGate)
{
    Rig r;

    using R = SoundcheckController::Refusal;

    auto noCeiling = r.params();
    noCeiling.ceilingDb = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ (r.armWith (noCeiling), R::InvalidParams);

    EXPECT_EQ (r.armWith (r.params (0.0f)), R::InvalidParams);

    auto noRate = r.params();
    noRate.sampleRate = 0.0;
    EXPECT_EQ (r.armWith (noRate), R::InvalidParams);

    EXPECT_EQ (r.sc.arm ({}, r.params(), risk (false, 0.0f)), R::NoChannels);

    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    r.pump (1000.0);
    EXPECT_EQ (r.peakOn (0), 0.0f);
    EXPECT_EQ (r.peakOn (1), 0.0f);
    EXPECT_FALSE (sawEvent (r.log, "soundcheck_start"));
}

// ------------------------------------------------------------ RUN SHAPE ----

// RED IF: the channels are measured simultaneously instead of one at a time.
// Two sweeps at once measures neither loop. inv 5.
//
// The room is SILENT here on purpose. peakOn() sees whatever leaves the
// channel, and an un-muted lane passes the mic straight through, so with a
// noisy fixture "channel 1 is not zero" would be true throughout the run and
// say nothing about the sweep.
TEST (SoundcheckController, SequencesOneOutputChannelAtATime)
{
    Rig r;                                   // micSource empty == digital silence
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);

    bool everBoth = false;
    const double perBlock = r.frames / kSr * 1000.0;
    for (double t = 0.0; t < 12000.0; t += perBlock)
    {
        r.block(); r.clock.advance (perBlock); r.sc.runOnce();
        if (r.peakOn (0) > 0.0f && r.peakOn (1) > 0.0f) everBoth = true;
    }
    EXPECT_FALSE (everBoth);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Results);
}

// RED IF: the run pushes anything into a command ring. Rev 1 only asserted the
// detection FLAG; this reads the rings themselves. inv 11, F20.
TEST (SoundcheckController, NoNotchCommandIsEmittedDuringARun)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);

    std::array<std::size_t, kMaxSlots> before {};
    for (int s = 0; s < kMaxSlots; ++s)
        before[(std::size_t) s] = r.engine.getCommandQueue (s).getAvailableRead();

    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    r.pump (12000.0);

    for (int s = 0; s < kMaxSlots; ++s)
        EXPECT_EQ (r.engine.getCommandQueue (s).getAvailableRead(), before[(std::size_t) s])
            << "slot " << s;

    // ...and the QUEUE DEPTH ALONE IS NOT ENOUGH. The audio callback drains
    // every queue on every block, so a command written mid-run is consumed
    // before this test can see it and the depth reads zero either way
    // (mutation-checked: writing one NotchCommand inside analyseCurrentTarget
    // left the assertions above entirely green). What the run must not do is
    // change the FILTERS, so the chains themselves are what gets asserted.
    for (int s = 0; s < kMaxSlots; ++s)
        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            const auto& chain = r.engine.getNotchChainForTest (s, lane);
            for (int i = 0; i < NotchChain::MAX_NOTCHES; ++i)
                EXPECT_EQ (chain.getNotchInfo (i).state, NotchChain::NotchState::Idle)
                    << "slot " << s << " lane " << lane << " index " << i
                    << " -- the run placed a notch";
        }
}

// RED IF: detection comes back late. Rev 1 held it off for 60 s and a -24 notch
// lost two rungs while spec §3 claimed 0 dB. inv 12, F9.
TEST (SoundcheckController, DetectionIsRestoredBeforeResults)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    EXPECT_FALSE (r.detectionOn) << "Arm must disarm detection";

    const double perBlock = r.frames / kSr * 1000.0;
    for (double t = 0.0; t < 12000.0; t += perBlock)
    {
        r.block(); r.clock.advance (perBlock); r.sc.runOnce();
        if (r.sc.getState() == SoundcheckController::State::Results)
        {
            EXPECT_TRUE (r.detectionOn) << "Results entered with detection still off";
            break;
        }
    }
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Results);
    EXPECT_TRUE (r.detectionOn);

    // I-1, and THIS is the assertion that has teeth. The poll-and-look above
    // samples after runOnce() has returned, so swapping the two statements
    // inside finishRun() leaves it green. inv 12 is about ORDER: detection must
    // already be back when Results is published, so the restore records the
    // state it ran in and that state must not be Results.
    ASSERT_TRUE (r.detectionEverRestored);
    EXPECT_NE (r.stateWhenDetectionRestored, SoundcheckController::State::Results)
        << "detection was restored AFTER Results was published -- a GUI polling "
           "getState() would see a results screen over a disarmed detector";
}

// RED IF: kResultsTimeoutMs drifts back up. 20 s is the number lane G's release
// ladder can tolerate without losing a rung. F9.
TEST (SoundcheckController, ResultsTimeoutIsTwentySeconds)
{
    EXPECT_DOUBLE_EQ (SoundcheckController::kResultsTimeoutMs, 20000.0);

    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);

    // I-1, corrected: the deadline is measured FROM the moment Results is
    // entered. Pumping a fixed 12000 ms first would already have spent ~3 s of
    // the 20 s window, and the brief's follow-up pump of 19000 would then reach
    // 22 s -- past the timeout it was asserting had not fired.
    ASSERT_TRUE (r.pumpUntil (SoundcheckController::State::Results, 15000.0));

    r.pump (SoundcheckController::kResultsTimeoutMs - 1000.0);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Results);
    r.pump (2000.0);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: nothing is placed without ÁP DỤNG -- checked here at the controller's
// own boundary. It has no NotchController to place with, and that is the point.
// inv 13, inv 17.
TEST (SoundcheckController, NothingIsPlacedWithoutApply)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    r.pump (12000.0);
    ASSERT_EQ (r.sc.getState(), SoundcheckController::State::Results);

    for (int s = 0; s < kMaxSlots; ++s)
        EXPECT_EQ (r.engine.getCommandQueue (s).getAvailableRead(), 0u);

    r.sc.dismissRequested();
    r.pump (10.0);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    for (int s = 0; s < kMaxSlots; ++s)
        EXPECT_EQ (r.engine.getCommandQueue (s).getAvailableRead(), 0u);
}

// --------------------------------------------------- THE NOISE-FLOOR GATE --

// RED IF: the noise-floor gate is compared against kConfirmScore = 0.7 -- a
// 0..1 PRODUCT -- instead of against the live PEAKINESS RATIO. Every real room
// reads well under 10 on a quiet noise floor, so that mistake aborts every run
// everywhere. This test going green is the only thing that catches it. N1.
TEST (SoundcheckController, NoiseFloorOfAQuietRoomDoesNotAbort)
{
    Rig r;
    r.micSource = whiteNoise (16384, 1.0e-3f);

    ASSERT_EQ (r.armWith (r.params (10.0f)), SoundcheckController::Refusal::None);
    r.pump (2000.0);

    // FLAKE GUARD, and it must come FIRST.
    //
    // The 7.35 figure (PeakinessAnalyzer.h:60-65) is a 60-seed ONE-SHOT
    // maximum. Pinning the fixture first means a drifting fixture fails AS a
    // fixture problem, with a message that says so, instead of masquerading as
    // a broken gate.
    ASSERT_LT (r.sc.worstPeakinessForTest(), 10.0f)
        << "the synthetic noise floor drifted over the gate -- reseed the fixture, "
           "do not relax the gate";
    ASSERT_GT (r.sc.worstPeakinessForTest(), 0.0f)
        << "no noise window was scored at all -- the gate never ran";

    EXPECT_NE (r.sc.getState(), SoundcheckController::State::Idle);
    EXPECT_FALSE (sawEvent (r.log, "soundcheck_abort"));
}

// RED IF: the gate is removed, or raised so far that a genuinely ringing room
// gets swept anyway. N1.
//
// It also pins WHEN the gate decides: a ringing room must be refused with the
// sweep index still negative, i.e. before the first sample. That is what
// kNoiseFloorGuardMs buys, and asserting the output channel stayed at zero for
// the whole fixture is the only way to see it.
TEST (SoundcheckController, NoiseFloorWithARingingToneAborts)
{
    Rig r;
    r.micSource = noisePlusTone (16384, 1.0e-3f, 1000.0, 0.05f);

    ASSERT_EQ (r.armWith (r.params (10.0f)), SoundcheckController::Refusal::None);

    const double perBlock = r.frames / kSr * 1000.0;
    bool everArmed = false;
    for (double t = 0.0; t < 2000.0; t += perBlock)
    {
        r.block(); r.clock.advance (perBlock); r.sc.runOnce();
        if (r.engine.getSoundcheckOutputChannel() >= 0)
            everArmed = true;
        if (r.sc.getState() == SoundcheckController::State::Idle)
            break;
    }

    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "room_ringing");
    // C-1: the channel is the whole assertion. A peak of zero could just mean
    // the ramp had not opened yet; scOutChannel_ never leaving -1 means the
    // sweep was never armed, so there was no audio clock for the gate to race.
    EXPECT_FALSE (everArmed)
        << "the sweep was armed before the gate had said the room was quiet";
    expectEngineStoodDown (r);
}

// RED IF: the sweep is armed on a timer rather than on the gate's answer (C-1).
// The poll that makes the decision can be arbitrarily late -- a loaded message
// thread, a stall, a debugger -- and if scOutChannel_ were published at the
// start of the noise floor the callback would reach sample 0 on its own and
// sweep a room nobody has scored yet.
TEST (SoundcheckController, SweepIsNotArmedUntilTheGateSaysQuiet)
{
    Rig r;                              // silent room: zero really means zero
    ASSERT_EQ (r.armWith (r.params (10.0f)), SoundcheckController::Refusal::None);

    // The DEVICE keeps running; the lane M thread does not poll at all, for
    // 200 ms longer than the whole noise floor.
    r.driveUnpolled (SoundcheckController::kNoiseFloorMs + 200.0);

    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), -1)
        << "armed without a gate decision";
    EXPECT_FALSE (r.engine.soundcheckIsEmitting());
    EXPECT_EQ (r.peakOn (0), 0.0f);
    EXPECT_EQ (r.peakOn (1), 0.0f);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::NoiseFloor);

    // ...and one poll later, with the room scored quiet, it arms.
    r.pump (20.0);
    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), 0);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Sweep);
}

// RED IF: the sweep deadlines are carried forward from the noise-floor deadline
// instead of re-stamped when the sweep is actually armed (N-1).
//
// After C-1 the audio starts when the GATE POLL arms it, not when the noise
// floor was entered. A poll that is L late therefore leaves a deadline L short:
// under 700 ms that silently under-measures the tail (a resonance whose decay
// fell outside the window reads lower than it is -- the estimator cannot tell,
// and the log says nothing); past 700 ms the Gap arrives while the sweep is
// still at FULL AMPLITUDE and the bare release is a hard cut on a live PA.
TEST (SoundcheckController, LateGatePollDoesNotShortenTheTailOrCutTheSweep)
{
    Rig r;                          // silent room: any non-zero output IS the sweep
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);

    // 800 ms late -- past kTailSeconds, which is what turns the bug audible.
    r.driveUnpolled (SoundcheckController::kNoiseFloorMs + 800.0);
    ASSERT_EQ (r.engine.getSoundcheckOutputChannel(), -1);

    const double armedAtMs = r.clock.nowMs();
    r.sc.runOnce();                                    // the late gate poll
    ASSERT_EQ (r.sc.getState(), SoundcheckController::State::Sweep);
    ASSERT_EQ (r.engine.getSoundcheckOutputChannel(), 0);

    const double perBlock = r.frames / kSr * 1000.0;

    // (a) 3.0 s after ARMING -- i.e. just before the sweep audio itself ends --
    //     the machine must still be sweeping. The old arithmetic entered Gap at
    //     armedAt + 2.92 s, 100 ms before the audio finished.
    while (r.clock.nowMs() < armedAtMs + 3000.0)
    {
        r.block(); r.clock.advance (perBlock); r.sc.runOnce();
    }
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Sweep)
        << "the Sweep phase ended before the sweep audio did";
    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), 0);
    EXPECT_FALSE (r.engine.isSoundcheckRampOutPending())
        << "the sweep was cut short -- something asked for an emergency fade";
    EXPECT_GT (r.peakOn (0), 0.0f) << "nothing is being emitted at all";

    // (b) and no release by the CONTROLLER ever lands on a channel that is
    //     still making sound, for either target.
    float peakAtRelease = -1.0f;
    int   releases = 0;
    for (double t = 0.0; t < 12000.0; t += perBlock)
    {
        r.block();
        const float peakThisBlock  = r.peakOn (0) + r.peakOn (1);
        const bool  armedAfterBlock = r.engine.getSoundcheckOutputChannel() >= 0;
        r.clock.advance (perBlock);
        r.sc.runOnce();
        if (armedAfterBlock && r.engine.getSoundcheckOutputChannel() < 0)
        {
            ++releases;
            peakAtRelease = std::max (peakAtRelease, peakThisBlock);
        }
    }
    EXPECT_GT (releases, 0) << "the run never released a channel -- a fixture problem";
    EXPECT_FLOAT_EQ (std::max (peakAtRelease, 0.0f), 0.0f)
        << "a channel was released while it was still playing: that is a hard "
           "cut at whatever amplitude the sweep had reached";
}

// RED IF: the gate answers a question about the whole spectrum instead of the
// band the sweep measures (I-2). An LED driver at 17 kHz, a switch-mode supply
// or mains hum are narrow, permanent and outside [kSweepLowHz, kTrustedHighHz]:
// scoring them refuses every run in the venue and says nothing about whether the
// room rings where the measurement is going.
TEST (SoundcheckController, GateIgnoresBinsOutsideTheSweepBand)
{
    {
        Rig out;
        out.micSource = noisePlusTone (16384, 1.0e-3f, 17000.0, 0.05f);
        ASSERT_EQ (out.armWith (out.params (10.0f)), SoundcheckController::Refusal::None);
        out.pump (2000.0);
        EXPECT_FALSE (sawEvent (out.log, "soundcheck_abort"))
            << "a 17 kHz whine, outside the swept band, refused the run";
        EXPECT_NE (out.sc.getState(), SoundcheckController::State::Idle);
    }
    {
        Rig in;                                     // the control, in-band
        in.micSource = noisePlusTone (16384, 1.0e-3f, 1000.0, 0.05f);
        ASSERT_EQ (in.armWith (in.params (10.0f)), SoundcheckController::Refusal::None);
        in.pump (2000.0);
        EXPECT_TRUE (sawEvent (in.log, "soundcheck_abort"));
        EXPECT_EQ (abortReasonInLog (in.log), "room_ringing");
        in.micSource.assign (4096, 0.0f);
        expectEngineStoodDown (in);
    }
}

// RED IF: an unmeasured noise floor is read as a quiet one. No frames means the
// device delivered nothing at all -- and "we heard nothing" is not "the room is
// silent", it is "we were not listening". The gate FAILS CLOSED.
TEST (SoundcheckController, AnUnmeasurableNoiseFloorFailsClosed)
{
    Rig r;
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);

    // The clock runs past the gate deadline without a single callback.
    r.clock.advance (SoundcheckController::kNoiseFloorMs + 50.0);
    r.sc.runOnce();

    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "noise_floor_unmeasured");
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    expectEngineStoodDown (r);
}

// RED IF: the gate is hard-coded instead of read from the detector's live
// threshold. An operator who lowers the threshold for a difficult room must take
// lane M's gate with them, or the two numbers drift apart exactly as lane R's
// did. N1.
TEST (SoundcheckController, NoiseFloorGateIsReadAtArm)
{
    for (float gate : { 5.0f, 20.0f })
    {
        Rig r;
        // A tone whose peakiness lands BETWEEN the two gates, DERIVED rather
        // than nudged (lane G B-5). peakinessAt compares one bin against the
        // mean of the six at offsets +-3..+-5, and the gate reads the MEAN
        // power spectrum of the whole 0.52 s window, so:
        //
        //   noise power per bin  = sigma^2 * sum(w^2) = sigma^2 * 3N/8 = sigma^2 * 768
        //   tone  power in bin   = (A * sum(w)/2 * H(d))^2 = (A * 512 * 0.930)^2
        //                          H(0.333) = sinc(d)/(1-d^2) = 0.930, because
        //                          1 kHz at 48 kHz is bin 42.667 -- OFF bin
        //   peakiness            = sqrt(1 + 295.5 * (A/sigma)^2)
        //
        // Target 10 (a factor of 2 clear of BOTH gates):
        //   100 = 1 + 295.5 * rho^2  ->  rho = 0.579  ->  A = 5.8e-4 at sigma 1e-3.
        r.micSource = noisePlusTone (16384, 1.0e-3f, 1000.0, 5.8e-4f);

        ASSERT_EQ (r.armWith (r.params (gate)), SoundcheckController::Refusal::None);
        r.pump (2000.0);

        const float measured = r.sc.worstPeakinessForTest();
        ASSERT_GT (measured, 5.0f) << "fixture drifted below the low gate";
        ASSERT_LT (measured, 20.0f) << "fixture drifted above the high gate";

        if (gate == 5.0f)
            EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"))  << "gate 5 must abort";
        else
            EXPECT_FALSE (sawEvent (r.log, "soundcheck_abort")) << "gate 20 must not";
    }
}

// ---------------------------------------------------------------- ABORTS ---

// RED IF: the abort path waits for the controller thread. The thread is
// deliberately not polled after the request; only the callback runs. inv 9, F8.
//
// The room is SILENT so "the sweep stopped" can be asserted as EXACTLY zero:
// once the callback releases the channel the lane is un-muted again, and with a
// noisy fixture the programme passing through would make peakOn() non-zero for
// a reason that has nothing to do with the sweep.
TEST (SoundcheckController, AbortRampsDownInTheCallbackAlone)
{
    Rig r;
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    r.pump (1500.0);                                   // mid-sweep
    ASSERT_GT (r.peakOn (0), 0.0f);

    r.sc.requestStop (SoundcheckController::AbortReason::UserStop);

    for (int i = 0; i < 40; ++i) r.block();            // NO runOnce()
    EXPECT_FLOAT_EQ (r.peakOn (0), 0.0f);
    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), -1);

    // I-5, and this has to be sampled BEFORE the first poll: beginAbort() lifts
    // the taps as well, so anything measured after a runOnce() cannot tell
    // "requestStop did it" from "the poll did it".
    {
        auto& tap = r.engine.getTapBuffer (0, 0);
        std::vector<float> drain (8192, 0.0f);
        while (tap.read (drain.data(), drain.size()) > 0) {}
        r.block();
        EXPECT_GT (tap.getAvailableRead(), 0u)
            << "requestStop() left the taps suspended -- a deaf feedback killer "
               "for as long as it takes the lane M thread to notice";
    }

    // I-5: requestStop() also lifted the tap suspension itself -- the safe
    // direction, and it must not wait for a poll, because a suspended tap is a
    // deaf feedback killer. Detection is re-armed on the lane M thread, so it
    // takes the one runOnce() the machine has been denied so far.
    r.sc.runOnce();
    EXPECT_TRUE (r.detectionOn);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    expectEngineStoodDown (r);
}

// RED IF: arming is allowed on top of a fade that is still running. Arming
// calls setSoundcheckOutputChannel(), which DISCARDS a pending ramp-out (C-2) --
// so a fast re-arm turns the previous abort into the hard cut the ramp exists
// to prevent.
TEST (SoundcheckController, ArmRefusesWhileAFadeIsStillRunning)
{
    Rig r;
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    r.pump (1500.0);                                   // mid-sweep
    ASSERT_TRUE (r.engine.soundcheckIsEmitting());

    r.sc.requestStop (SoundcheckController::AbortReason::UserStop);
    r.sc.runOnce();
    ASSERT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    ASSERT_TRUE (r.engine.isSoundcheckRampOutPending())
        << "nothing is fading, so this test proves nothing";

    EXPECT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::RampOutPending);

    // Once the callback has finished the fade, the next run may start.
    for (int i = 0; i < 16; ++i) r.block();
    EXPECT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
}

// RED IF: abortAndJoin() runs the abort BEFORE it joins (C-2). Two threads in
// one state machine, and a poll thread sitting in Gap calls enterTarget() right
// after the caller has torn the run down -- arming a whole fresh sweep with the
// taps suspended and detection off, with nobody left watching.
//
// This is also the ONLY test that drives the real poll thread, so it is what
// covers start()/run()/stop() at all.
TEST (SoundcheckController, AbortAndJoinJoinsBeforeItRuns)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);

    r.sc.start();
    ASSERT_TRUE (r.sc.isPollThreadRunningForTest());

    // Let the REAL poll thread take the machine into the sweep. It wakes every
    // kPollMs of WALL time; the fake clock only moves when this thread moves
    // it, so the two have to be interleaved by hand.
    const double perBlock = r.frames / kSr * 1000.0;
    for (int i = 0; i < 120 && ! r.engine.soundcheckIsEmitting(); ++i)
    {
        for (int b = 0; b < 4; ++b) { r.block(); r.clock.advance (perBlock); }
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    ASSERT_TRUE (r.engine.soundcheckIsEmitting())
        << "the poll thread never armed the sweep -- the fixture, not the code";

    EXPECT_TRUE (r.sc.abortAndJoin());
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    EXPECT_FALSE (r.sc.isPollThreadRunningForTest())
        << "abortAndJoin() must leave the poll thread stopped; Task 10 restarts it";
    EXPECT_TRUE (r.detectionOn);
    expectEngineStoodDown (r);
}

// RED IF: destroying the controller mid-run leaves the engine armed (C-3).
// Stopping the THREAD is not stopping the RUN: the PA would keep the sweep
// until the callback ran out of signal, the taps would stay suspended and the
// feedback killer would never wake up again -- with the only object that could
// undo any of it already gone.
TEST (SoundcheckController, DestroyingMidRunStandsTheEngineDown)
{
    Rig r;                       // r.sc stays Idle; the run under test is its own
    bool detection = true;
    {
        SoundcheckController extra { r.engine, r.clock };
        extra.setDetectionActiveOnAllSlots = [&detection] (bool on) { detection = on; };
        ASSERT_EQ (extra.arm (r.stereoTargets(), r.params(), risk (false, 0.0f)),
                   SoundcheckController::Refusal::None);
        ASSERT_FALSE (detection) << "arm must disarm detection";

        const double perBlock = r.frames / kSr * 1000.0;
        for (double t = 0.0; t < 1500.0; t += perBlock)
        {
            r.block(); r.clock.advance (perBlock); extra.runOnce();
        }
        ASSERT_TRUE (r.engine.soundcheckIsEmitting()) << "not mid-sweep";
    }   // <-- the destructor is the code under test

    EXPECT_TRUE (detection) << "the destructor left the detector disarmed";
    expectEngineStoodDown (r);
}

// RED IF: an abort with a dead callback hands the stop to the callback (S-2).
// requestSoundcheckRampOut() only sets a flag; with the device stopped no block
// will ever read it, so the channel stays armed for ever and the NEXT device to
// open inherits an armed soundcheck. There is nothing to click either, so the
// backstop is free here -- and it is the only thing that works.
TEST (SoundcheckController, EngineStoppedAbortReleasesTheChannelWithNoCallback)
{
    Rig r;
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    r.pump (1500.0);                                   // mid-sweep
    ASSERT_TRUE (r.engine.soundcheckIsEmitting());

    r.setRunning (false);
    r.sc.runOnce();

    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "engine_stopped");
    // NOT ONE further callback has run, and the channel is already released.
    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), -1)
        << "the abort waited for a fade that nothing will ever generate";
    EXPECT_FALSE (r.engine.isSoundcheckRampOutPending());
    EXPECT_TRUE (r.detectionOn);
    r.setRunning (true);        // the helper drives blocks; put the rig back first
    expectEngineStoodDown (r);
}

// RED IF: a device error is not an abort. getLastDeviceError() is the only
// channel through which a driver failure reaches this thread at all.
TEST (SoundcheckController, DeviceErrorAborts)
{
    Rig r;
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    r.pump (200.0);
    ASSERT_FALSE (sawEvent (r.log, "soundcheck_abort"));

    r.engine.audioDeviceError ("the interface fell over");
    r.sc.runOnce();

    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    EXPECT_TRUE (r.detectionOn);
    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), -1);
    expectEngineStoodDown (r);
}

// RED IF: micCaptureDrops_ is ignored. A drop splices sample N onto N+k, which
// through the Hann window is broadband energy in every bin -- the measurement is
// worthless and must not be reported as a measurement. F20.
TEST (SoundcheckController, CaptureDropAborts)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);

    // Fill the capture ring by driving the callback without ever polling.
    for (int i = 0; i < 400; ++i)
    {
        r.block();
        r.clock.advance (r.frames / kSr * 1000.0);
    }
    ASSERT_GT (r.engine.getMicCaptureDropCount(), 0u);

    r.sc.runOnce();
    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "capture_drop");
    EXPECT_TRUE (r.detectionOn);
    expectEngineStoodDown (r);
}

// RED IF: the sample rate recorded at Preflight is not re-checked. A rate change
// invalidates T, the reference spectrum X and every bin-to-Hz mapping at once,
// and getLastDeviceError() reports NOTHING in that case. inv 20, F15.
TEST (SoundcheckController, SampleRateChangeAborts)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    auto p = r.params(); p.sampleRate = 44100.0;       // NOT the engine's rate
    ASSERT_EQ (r.armWith (p), SoundcheckController::Refusal::None);

    r.pump (50.0);
    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "device_changed");
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    EXPECT_TRUE (r.detectionOn);
    expectEngineStoodDown (r);
}

// RED IF: the channel counts recorded at Preflight are not re-checked. A restart
// onto fewer channels with the sweep still armed is the out-of-bounds case
// AudioEngine's per-callback bounds check catches -- but the RUN must end too,
// not silently produce nothing. inv 20, F15.
TEST (SoundcheckController, ChannelCountChangeAborts)
{
    Rig r { 4 };
    r.micSource = whiteNoise (4096, 1.0e-4f);
    auto p = r.params(); p.numOutputChannels = 8;      // claims more than the callback delivers
    ASSERT_EQ (r.armWith (p), SoundcheckController::Refusal::None);

    r.pump (50.0);
    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "device_changed");
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    EXPECT_TRUE (r.detectionOn);
    expectEngineStoodDown (r);
}

// RED IF: the hot-mic abort is dropped or its hold time is not enforced. A
// single over-threshold block must NOT abort; kMicAbortHoldMs of them must.
// Q3.
TEST (SoundcheckController, HotMicAbortsOnlyAfterTheHold)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    r.pump (600.0);
    ASSERT_FALSE (sawEvent (r.log, "soundcheck_abort"));

    // THE HOLD, first. One 5.33 ms block over the threshold is SHORTER than
    // kMicAbortHoldMs = 20 ms and must NOT abort. Without this half, the test
    // passes for an implementation that aborts on the first hot sample -- which
    // would kill a run on any transient.
    r.micSource.assign (4096, 0.9f);
    r.pump (5.4);
    r.micSource = whiteNoise (4096, 1.0e-4f);
    r.pump (200.0);
    ASSERT_FALSE (sawEvent (r.log, "soundcheck_abort"))
        << "one short burst is not a hot mic";

    // Now hold it well past the hold: -6 dBFS is 0.5, and 0.9 is comfortably over.
    r.micSource.assign (4096, 0.9f);
    r.pump (200.0);

    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "mic_hot");
    EXPECT_TRUE (r.detectionOn);
    // The room is still screaming at 0.9 here; what must be true is that the
    // soundcheck side of the engine has let go of it.
    r.micSource.assign (4096, 0.0f);
    expectEngineStoodDown (r);
}

// RED IF: ring risk is only read before the Confirm dialog (S-1). Preflight can
// be minutes old by the time the operator presses OK, and a room that started
// ringing in between is exactly the room that must not be swept.
TEST (SoundcheckController, ArmRefusesWhenRingRiskRoseDuringConfirm)
{
    Rig r;
    ASSERT_EQ (r.sc.preflight (r.stereoTargets(), risk (true, 0.3f)),
               SoundcheckController::Refusal::None);

    // 0.55 * 0.7 = 0.385, and the room crossed it while the dialog was up.
    EXPECT_EQ (r.armWith (r.params(), risk (true, 0.4f)),
               SoundcheckController::Refusal::RingRiskRising);

    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    EXPECT_FALSE (sawEvent (r.log, "soundcheck_start"));
    r.pump (1000.0);
    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), -1);
}

// RED IF: arm() trusts preflight's view of the routing (I-4). A slot can be
// switched off, or re-patched, while the Confirm dialog is on screen.
TEST (SoundcheckController, ArmRefusesATargetDisabledDuringConfirm)
{
    Rig r;
    ASSERT_EQ (r.sc.preflight (r.stereoTargets(), risk (false, 0.0f)),
               SoundcheckController::Refusal::None);

    SlotConfig c = r.engine.getSlotConfig (0); c.enabled = false;
    r.engine.setSlotConfig (0, c);

    EXPECT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::SlotDisabled);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
    EXPECT_FALSE (sawEvent (r.log, "soundcheck_start"));
    r.pump (1000.0);
    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), -1);
}

// RED IF: the progress readouts stop tracking the run. They are the only thing
// standing between the operator and 72 s of a progress bar that says nothing,
// and a remaining-time that runs backwards is worse than none.
TEST (SoundcheckController, ProgressReadsTrackTheRun)
{
    Rig r;
    EXPECT_EQ (r.sc.getElapsedMsInRun(), 0.0);

    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    EXPECT_EQ (r.sc.getTargetCount(), 2);
    EXPECT_EQ (r.sc.getCurrentTargetIndex(), 0);

    r.pump (1000.0);                       // inside the FIRST channel
    EXPECT_EQ (r.sc.getCurrentTargetIndex(), 0);
    const double earlyElapsed   = r.sc.getElapsedMsInRun();
    const double earlyRemaining = r.sc.getRemainingMsInRun();
    EXPECT_GT (earlyElapsed, 900.0);
    EXPECT_LT (earlyRemaining, 2.0 * SoundcheckController::kPerTargetMs);

    r.pump (SoundcheckController::kPerTargetMs);    // into the SECOND
    EXPECT_EQ (r.sc.getCurrentTargetIndex(), 1);
    EXPECT_GT (r.sc.getElapsedMsInRun(), earlyElapsed);
    EXPECT_LT (r.sc.getRemainingMsInRun(), earlyRemaining);

    ASSERT_TRUE (r.pumpUntil (SoundcheckController::State::Results, 15000.0));
    EXPECT_EQ (r.sc.getRemainingMsInRun(), 0.0);
}

// RED IF: two abort reasons collapse onto one string. The log is the only place
// a soundman can find out why a run stopped, and "device_changed" and
// "capture_drop" ask for two different things to be checked. I-7.
TEST (SoundcheckController, EveryAbortReasonHasItsOwnName)
{
    using A = SoundcheckController::AbortReason;
    const A all[] { A::UserStop, A::Esc, A::EngineStopped, A::DeviceError,
                    A::DeviceChanged, A::MicHot, A::RoomRinging, A::CaptureDrop,
                    A::NoiseFloorUnmeasured };

    for (const A a : all)
    {
        const juce::String name { SoundcheckController::abortReasonNameForTest (a) };
        EXPECT_FALSE (name.isEmpty());
        EXPECT_NE (name, "unknown") << "a new enumerator has no name";
        for (const A b : all)
            if (a != b)
                EXPECT_NE (name,
                           juce::String (SoundcheckController::abortReasonNameForTest (b)));
    }
}

// --------------------------------------------------------------- RESULTS ---

// RED IF: a channel that could not be measured is reported as a flat 0 dB
// margin, which reads on screen as "a very good room". Q8, F26.
TEST (SoundcheckController, UnmeasurableChannelIsAValidResultNotAFlatLine)
{
    Rig r;
    r.micSource.assign (4096, 0.0f);            // dead mic: nothing comes back
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    r.pump (12000.0);

    const auto results = r.sc.copyResults();
    ASSERT_FALSE (results.empty());
    EXPECT_FALSE (results[0].measured);
    EXPECT_FALSE (results[0].routingInvalid);
    EXPECT_EQ (results[0].candidateCount, 0);
    // ...and NOT because the ceiling or the ladder was missing. Those two are
    // caller bugs that also produce an empty proposal list, and an operator
    // must never see them as "the room is clean" (Task 3 I-3).
    EXPECT_FALSE (results[0].ceilingMissing);
    EXPECT_FALSE (results[0].ladderMissing);
}

// RED IF: the per-slot view stops filtering. Task 10's APPLY walks the slots and
// hands each slot's own results to that slot's NotchController; a leak across
// slots would place one slot's proposals on another's chain. I-4.
TEST (SoundcheckController, ResultsAreReadableWholeAndPerSlot)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_EQ (r.armWith (r.params()), SoundcheckController::Refusal::None);
    ASSERT_TRUE (r.pumpUntil (SoundcheckController::State::Results, 15000.0));

    EXPECT_EQ ((int) r.sc.copyResults().size(), 2);
    EXPECT_EQ ((int) r.sc.copyResultsForSlot (0).size(), 2);
    EXPECT_EQ ((int) r.sc.copyResultsForSlot (1).size(), 0);
    for (const auto& res : r.sc.copyResultsForSlot (0))
        EXPECT_EQ (res.slot, 0);
}

// ============================================================ TASK 7: APPLY ==
//
// applySoundcheckResults is the ONLY place lane M's proposals become real
// notches, and it is MESSAGE THREAD ONLY (spec 4.6e, inv 17).

// RED IF: applySoundcheckResults stops refusing to run off the message thread.
// setNotchImpl reads width_ (NotchController.cpp:199) and the detector's sample
// rate (:209) OUTSIDE modelMutex_, so setNotch/clearNotch are message-thread by
// contract (NotchController.h:219-224). inv 17, F27.
//
// The brief's version of this test only compared this_thread::get_id() with
// itself before and after the call -- a tautology that cannot fail whatever the
// implementation does. This one actually calls it from ANOTHER thread and
// asserts nothing was written; drop the guard and it goes red.
TEST (SoundcheckApply, ApplyRunsOnTheMessageThread)
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // the MessageManager is THIS thread's
    ASSERT_TRUE (juce::MessageManager::existsAndIsCurrentThread());

    NotchRig rig { 1 };
    const std::vector<SoundcheckController::OutputResult> results {
        oneCandidate (0, 0, 1000.0, -12.0) };

    SoundcheckApplyStats offThread;
    std::thread t ([&] { offThread = applySoundcheckResults (*rig.controller, results); });
    t.join();

    EXPECT_EQ (offThread.placed, 0) << "nothing may be written off the message thread";
    EXPECT_EQ (offThread.clearedPrevious, 0);
    EXPECT_GE (offThread.refused, 1) << "and the refusal is REPORTED, not silent";
    EXPECT_FALSE (rig.controller->activeForTest (0, 15));

    // ...and the same call on the message thread does place.
    const auto stats = applySoundcheckResults (*rig.controller, results);
    EXPECT_EQ (stats.placed, 1);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15)) << "top-down allocation starts at 15";
}

// RED IF: either half of B-1 is dropped -- the takenThisCall bitmap, or the
// per-setNotch re-read. The bitmap is what makes six placements land on six
// indices (the snapshot refreshes at hop cadence, ~10.7 ms, on the DETECTOR
// thread, while this loop runs in microseconds on the message thread, so every
// re-read returns the same frame). The re-read is what catches a detector
// placement that landed since entry. F10, B-1.
TEST (SoundcheckApply, ApplyReReadsTheSnapshotBeforeEachSetNotch)
{
    NotchRig rig { 1 };

    std::vector<SoundcheckController::OutputResult> results;
    auto r = oneCandidate (0, 0, 1000.0, -12.0);
    r.candidateCount = 2;
    r.candidates[1].hz = 2000.0f; r.candidates[1].q = 30.0f; r.candidates[1].depthDb = -12.0f;
    results.push_back (r);

    // Occupy index 14 with a DETECTOR notch before the call, so the re-read has
    // something real to catch. The takenThisCall bitmap handles 15; the re-read
    // handles 14. Both halves are exercised by this one test.
    ASSERT_TRUE (rig.controller->setNotch (0, 14, 5000.0, 30.0, -12.0,
                                           NotchController::Origin::Detector));
    rig.pump();

    const auto stats = applySoundcheckResults (*rig.controller, results);

    EXPECT_EQ (stats.placed, 2);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15));
    EXPECT_TRUE (rig.controller->activeForTest (0, 14)) << "the detector's notch is still here";
    EXPECT_TRUE (rig.controller->activeForTest (0, 13))
        << "14 was taken by the detector, and 15 by THIS call -- 13 is next. "
           "Without takenThisCall (B-1) the second placement lands on 15 again "
           "and overwrites the first, because the snapshot cannot have refreshed "
           "in the microseconds between the two setNotch calls.";
    EXPECT_NEAR (rig.controller->depthDbForTest (0, 14), -12.0, 1.0e-9);
}

// RED IF: an INDEP failure rolls back. Each INDEP notch stands alone, so a notch
// that WAS placed is real protection and pulling it because the next one failed
// is strictly worse. N4.
TEST (SoundcheckApply, IndepApplyDoesNotUnwind)
{
    NotchRig rig { 2 };
    rig.controller->setLinked (false);

    std::vector<SoundcheckController::OutputResult> results {
        oneCandidate (0, 0, 1000.0, -12.0),
        oneCandidate (0, 1, 1500.0, -12.0)
    };

    rig.controller->failNextSetNotchOnLaneForTest (1);
    const auto stats = applySoundcheckResults (*rig.controller, results);

    EXPECT_EQ (stats.placed, 1);
    EXPECT_EQ (stats.refused, 1);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15)) << "the lane-0 notch STAYS";
}

// RED IF: an INDEP failure keeps going and silently loses count. Stop at the
// first false, and report it. F10.
TEST (SoundcheckApply, PartialApplyStopsAndReportsRefused)
{
    NotchRig rig { 1 };

    auto r = oneCandidate (0, 0, 1000.0, -12.0);
    r.candidateCount = 3;
    // An invalid Q makes setNotchImpl refuse at NotchController.cpp:211.
    r.candidates[1].hz = 2000.0f; r.candidates[1].q = 0.0f;  r.candidates[1].depthDb = -12.0f;
    r.candidates[2].hz = 3000.0f; r.candidates[2].q = 30.0f; r.candidates[2].depthDb = -12.0f;

    const auto stats = applySoundcheckResults (*rig.controller, { r });

    EXPECT_EQ (stats.placed, 1);
    EXPECT_GE (stats.refused, 1);
    EXPECT_FALSE (rig.controller->activeForTest (0, 13)) << "it must STOP, not skip and carry on";
}

// RED IF: a linked slot gets one lane only, or two different indices. The
// detector's LINKED path (firstFreeIndexAllLanesLocked, NotchController.cpp:
// 1072-1083) would then never find an index free on both lanes and would
// silently slip. F16.
TEST (SoundcheckApply, LinkedSlotPlacesBothLanesAtOneIndex)
{
    NotchRig rig { 2 };                   // width 2 AND a real lane-1 tap
    rig.controller->setLinked (true);

    // B-1(b): laneCount is published from analysedLanes() and defaults to 1.
    // If the snapshot has never refreshed -- or refreshed without a lane-1
    // block -- lane M reads laneCount == 1 and places ONE lane on a LINKED
    // slot. NotchRig::pump() writes BOTH taps; assert the precondition rather
    // than assuming it.
    const auto pre = rig.snapshot();
    ASSERT_EQ (pre.laneCount, 2u) << "pump both taps, or this test cannot fail correctly";

    const auto stats = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -12.0) });

    EXPECT_EQ (stats.placed, 2);
    EXPECT_TRUE (rig.controller->activeForTest (0, 15));
    EXPECT_TRUE (rig.controller->activeForTest (1, 15));
    EXPECT_NEAR (rig.controller->depthDbForTest (0, 15),
                 rig.controller->depthDbForTest (1, 15), 1.0e-9);
}

// RED IF: linkedness is read from SnapshotBuffer::linked alone. That field is
// the operator's SWITCH, not the behaviour (NotchController.cpp:637-640): a
// mono slot is FORCED linked by effectiveLinked() while the switch still reads
// INDEP. N5.
TEST (SoundcheckApply, LinkedIsDerivedFromLaneCountNotJustTheSwitch)
{
    NotchRig rig { 1 };                   // mono: laneCount == 1, linked switch OFF
    rig.controller->setLinked (false);

    const auto snap = rig.snapshot();
    ASSERT_FALSE (snap.linked);
    ASSERT_LT (snap.laneCount, 2u);

    const auto stats = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -12.0) });

    // One lane exists, so "every driven lane" is one placement -- and crucially
    // NOTHING is written to lane 1, which has no tap.
    EXPECT_EQ (stats.placed, 1);
    EXPECT_TRUE  (rig.controller->activeForTest (0, 15));
    EXPECT_FALSE (rig.controller->activeForTest (1, 15));
}

// RED IF: a linked pair is left half-placed. One lane protected while the GUI
// claims both is worse than placing nothing -- the same reasoning
// placeConfirmed and adoptPreset already follow. N4.
TEST (SoundcheckApply, LinkedPairUnwindsWhenTheSecondLaneFails)
{
    // N-1: the RECORDER FIRST. Destruction runs in reverse declaration order,
    // so a recorder declared after the rig is already gone when
    // ~NotchController runs stop() and flushes its remaining events through the
    // sink -- a write into a destroyed vector.
    EventRecorder rec;
    NotchRig      rig { 2 };
    rig.controller->setLinked (true);
    rig.controller->setEventSink (rec.sink());
    // B-1(c) again, and this one is not only a fixture detail: SnapshotBuffer::
    // linked is republished ONLY inside runOnce() drain loop, so the operator
    // switch is invisible to a reader of the snapshot until a block has been
    // drained through it (up to one hop, ~10.7 ms). Without this pump the rig
    // still reads INDEP, applySoundcheckResults takes the INDEP branch and the
    // unwind this test exists to prove is never reached.
    ASSERT_TRUE (rig.snapshot().linked) << "the LINK switch has not been published yet";

    rig.controller->failNextSetNotchOnLaneForTest (1);
    const auto stats = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -12.0) });

    EXPECT_EQ (stats.placed, 0);
    EXPECT_GE (stats.refused, 1);
    EXPECT_FALSE (rig.controller->activeForTest (0, 15)) << "lane 0 must be unwound";

    rig.pump();
    EXPECT_TRUE (rec.sawClearWithReason (NotchController::ClearReason::PartialApplyUnwind))
        << "and with THAT reason, not Manual";
}

// RED IF: the previous run's preventive notches are left in place. They never
// auto-release (KD-7, NotchController.cpp:729), so the 16-slot chain drains
// after a few soundchecks. Spec 4.6b.
TEST (SoundcheckApply, ARerunReplacesItsOwnPreviousProposals)
{
    EventRecorder rec;                    // N-1: the RECORDER FIRST
    NotchRig      rig { 1 };
    rig.controller->setEventSink (rec.sink());

    const auto first = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -12.0) });
    ASSERT_EQ (first.placed, 1);
    rig.pump();                                   // B-1(c)

    const auto second = applySoundcheckResults (*rig.controller,
                                                { oneCandidate (0, 0, 1200.0, -12.0) });
    rig.pump();

    EXPECT_EQ (second.clearedPrevious, 1);
    EXPECT_EQ (second.placed, 1);
    EXPECT_TRUE (rec.sawClearWithReason (NotchController::ClearReason::SoundcheckReplace));
    EXPECT_EQ (rig.countSoundcheckNotches(), 1) << "exactly ONE preventive notch survives";
}

// RED IF: a re-run leaves the OTHER lane's stale proposal behind. I-12's
// ruling: a re-run replaces the whole SLOT, not one lane. A soundcheck measures
// a slot's outputs together and a LINKED placement writes both lanes at one
// index, so half-replacing produces a pair the operator never asked for. The
// mono test above cannot tell the difference; this one can.
TEST (SoundcheckApply, ARerunReplacesTheWholeSlotNotJustOneLane)
{
    NotchRig rig { 2 };
    rig.controller->setLinked (false);          // INDEP: two independent lanes

    ASSERT_EQ (applySoundcheckResults (*rig.controller,
                                       { oneCandidate (0, 0, 1000.0, -12.0),
                                         oneCandidate (0, 1, 1500.0, -12.0) }).placed, 2);
    rig.pump();

    // A re-run that names only lane 0 must still clear BOTH previous proposals.
    const auto again = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1100.0, -12.0) });
    rig.pump();

    EXPECT_EQ (again.clearedPrevious, 2) << "the whole slot, not just this lane";
    EXPECT_EQ (rig.countSoundcheckNotches(), 1);
}

// RED IF: a Detector or Preset notch is cleared by the replace pass. Only lane
// M own previous proposals go.
TEST (SoundcheckApply, ReplacingLeavesEveryOtherOriginAlone)
{
    NotchRig rig { 1 };
    ASSERT_TRUE (rig.controller->setNotch (0, 0, 500.0, 30.0, -12.0,
                                           NotchController::Origin::Detector));
    ASSERT_TRUE (rig.controller->setNotch (0, 1, 700.0, 30.0, -12.0,
                                           NotchController::Origin::Preset));
    ASSERT_TRUE (rig.controller->setNotch (0, 2, 900.0, 30.0, -12.0,
                                           NotchController::Origin::Manual));
    rig.pump();

    applySoundcheckResults (*rig.controller, { oneCandidate (0, 0, 1000.0, -12.0) });
    rig.pump();
    const auto again = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1100.0, -12.0) });

    EXPECT_EQ (again.clearedPrevious, 1);
    EXPECT_TRUE (rig.controller->activeForTest (0, 0));
    EXPECT_TRUE (rig.controller->activeForTest (0, 1));
    EXPECT_TRUE (rig.controller->activeForTest (0, 2));
}

// RED IF: the ternary at NotchController.cpp:243-245 changes and a preventive
// notch stops being its own ceiling. It is implicit behaviour, so it gets an
// explicit test. Spec 4.6d.
TEST (SoundcheckApply, APreventiveNotchIsItsOwnCeiling)
{
    NotchRig rig { 1 };
    applySoundcheckResults (*rig.controller, { oneCandidate (0, 0, 1000.0, -12.0) });

    EXPECT_DOUBLE_EQ (rig.controller->rawCeilingDbForTest (0, 15), -12.0);
    EXPECT_DOUBLE_EQ (rig.controller->ceilingDbForTest (0, 15), -12.0);
}

// RED IF: a result that could not be measured still places something. inv 13.
TEST (SoundcheckApply, AnUnmeasuredResultPlacesNothing)
{
    NotchRig rig { 1 };
    auto r = oneCandidate (0, 0, 1000.0, -12.0);
    r.measured = false;

    const auto stats = applySoundcheckResults (*rig.controller, { r });
    EXPECT_EQ (stats.placed, 0);
    EXPECT_FALSE (rig.controller->activeForTest (0, 15));
}

// ----------------------------------------------- invariant 15: +-1 FFT bin ---
//
// Task 6 hands SoundcheckCandidates::pick an EMPTY liveNotchHz list on purpose
// (inv 17: the lane M thread may not read a NotchController). So the whole of
// inv 15 -- "a proposal must never land within +-1 bin of a live notch on that
// lane" -- is enforced HERE, on the message thread, against the snapshot this
// function is already reading.
//
// RED IF: the +-1-bin skip is dropped. Two notches one bin apart are two
// filters doing one filter's job: the second buys almost no extra attenuation,
// and it costs a slot out of sixteen that a different howl will need. At 48 kHz
// with a 2048-point transform a bin is 23.44 Hz, so 1000 Hz and 1010 Hz are the
// SAME bin (43).
TEST (SoundcheckApply, AProposalWithinOneBinOfALiveNotchIsSkipped)
{
    NotchRig rig { 1 };
    ASSERT_TRUE (rig.controller->setNotch (0, 10, 1000.0, 30.0, -12.0,
                                           NotchController::Origin::Detector));
    rig.pump();
    ASSERT_GT (rig.snapshot().sampleRate, 0.0) << "hzToBin needs the published rate";

    auto r = oneCandidate (0, 0, 1010.0, -12.0);     // bin 43, same as the live notch
    r.candidateCount = 2;
    r.candidates[1].hz = 4000.0f;                    // bin 171 -- nowhere near
    r.candidates[1].q  = 30.0f;
    r.candidates[1].depthDb = -12.0f;

    const auto stats = applySoundcheckResults (*rig.controller, { r });

    EXPECT_EQ (stats.skippedLive, 1) << "the 1010 Hz proposal sits on the live notch";
    EXPECT_EQ (stats.placed, 1)      << "and the 4000 Hz one still lands";
    EXPECT_TRUE  (rig.controller->activeForTest (0, 15));
    EXPECT_FALSE (rig.controller->activeForTest (0, 14))
        << "a skipped proposal must not burn an index either";
    EXPECT_NEAR (rig.controller->depthDbForTest (0, 10), -12.0, 1.0e-9)
        << "and the live notch itself is untouched";
}

// RED IF: the skip is applied to notches THIS CALL has just cleared. A second
// soundcheck of the same room finds the same frequencies -- that is the normal
// case, not the odd one. The entry snapshot still lists a cleared notch for up
// to one hop (~10.7 ms), so without excluding what this call cleared, every
// re-run would refuse to re-place anything and the operator would be told the
// room had changed when nothing had.
TEST (SoundcheckApply, ARerunMayRePlaceAtTheSameFrequencyItJustCleared)
{
    NotchRig rig { 1 };
    ASSERT_EQ (applySoundcheckResults (*rig.controller,
                                       { oneCandidate (0, 0, 1000.0, -12.0) }).placed, 1);
    rig.pump();

    const auto again = applySoundcheckResults (*rig.controller,
                                               { oneCandidate (0, 0, 1000.0, -18.0) });

    EXPECT_EQ (again.clearedPrevious, 1);
    EXPECT_EQ (again.skippedLive, 0) << "a notch this call just cleared is not LIVE";
    EXPECT_EQ (again.placed, 1);
}
