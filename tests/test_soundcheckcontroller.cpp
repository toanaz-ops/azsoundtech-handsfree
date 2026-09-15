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
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;

class FakeClock : public ClockSource
{
public:
    double nowMs() const override { return ms_; }
    void advance (double m) { ms_ += m; }
private:
    double ms_ = 1000.0;
};

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
    std::vector<juce::var> log;

    explicit Rig (int chans = 2) : channels (chans)
    {
        in.assign  ((std::size_t) channels, std::vector<float> ((std::size_t) frames, 0.0f));
        out.assign ((std::size_t) channels, std::vector<float> ((std::size_t) frames, 0.0f));
        for (auto& v : in)  inPtr.push_back (v.data());
        for (auto& v : out) outPtr.push_back (v.data());

        sc.setDetectionActiveOnAllSlots = [this] (bool on) { detectionOn = on; ++detectionCalls; };
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

NotchController::SnapshotBuffer risk (bool valid, float score)
{
    NotchController::SnapshotBuffer s {};
    s.ringRiskValid     = valid;
    s.ringRiskScore     = score;
    s.ringRiskThreshold = 0.7f;      // what NotchController.cpp:648 publishes
    return s;
}

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

juce::String abortReasonInLog (const std::vector<juce::var>& log)
{
    for (const auto& v : log)
        if (evOf (v) == "soundcheck_abort")
            return v.getDynamicObject()->getProperty ("reason").toString();
    return {};
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
    ASSERT_TRUE (r.sc.arm (targets, r.params()));
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

    auto noCeiling = r.params();
    noCeiling.ceilingDb = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE (r.sc.arm (r.stereoTargets(), noCeiling));

    auto noGate = r.params (0.0f);
    EXPECT_FALSE (r.sc.arm (r.stereoTargets(), noGate));

    auto noRate = r.params();
    noRate.sampleRate = 0.0;
    EXPECT_FALSE (r.sc.arm (r.stereoTargets(), noRate));

    EXPECT_FALSE (r.sc.arm ({}, r.params()));

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
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));

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

    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
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
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
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
}

// RED IF: kResultsTimeoutMs drifts back up. 20 s is the number lane G's release
// ladder can tolerate without losing a rung. F9.
TEST (SoundcheckController, ResultsTimeoutIsTwentySeconds)
{
    EXPECT_DOUBLE_EQ (SoundcheckController::kResultsTimeoutMs, 20000.0);

    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));

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
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
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

    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (10.0f)));
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

    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (10.0f)));

    const double perBlock = r.frames / kSr * 1000.0;
    float loudest = 0.0f;
    for (double t = 0.0; t < 2000.0; t += perBlock)
    {
        r.block(); r.clock.advance (perBlock);
        // ONLY while the channel is still armed. Once the abort releases it the
        // lane is un-muted again and out[0] carries the room's own tone -- which
        // is the fixture, not the sweep, and measuring it would fail the test
        // for the very behaviour it exists to prove.
        if (r.engine.getSoundcheckOutputChannel() >= 0)
            loudest = std::max (loudest, r.peakOn (0));
        r.sc.runOnce();
        if (r.sc.getState() == SoundcheckController::State::Idle)
            break;
    }

    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "room_ringing");
    EXPECT_EQ (loudest, 0.0f)
        << "the gate decided AFTER the sweep had already started -- "
           "kNoiseFloorGuardMs is what keeps the decision ahead of the sound";
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

        ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (gate)));
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

// RED IF: the gate is re-read per channel instead of frozen in RunParams.
// Channel 1 and channel 2 of one measurement would then be scored on two
// different rulers, with nothing in the log saying so. Round 3, R3-1.
TEST (SoundcheckController, NoiseFloorGateIsStableWithinARun)
{
    Rig r;
    r.micSource = whiteNoise (16384, 1.0e-3f);

    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (20.0f)));
    r.pump (5000.0);                       // well into the second channel

    // Whatever an owner does to the detector's threshold now, this run keeps 20.
    r.pump (7000.0);
    EXPECT_FALSE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Results);
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
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    r.pump (1500.0);                                   // mid-sweep
    ASSERT_GT (r.peakOn (0), 0.0f);

    r.sc.requestStop (SoundcheckController::AbortReason::UserStop);

    for (int i = 0; i < 40; ++i) r.block();            // NO runOnce()
    EXPECT_FLOAT_EQ (r.peakOn (0), 0.0f);
    EXPECT_EQ (r.engine.getSoundcheckOutputChannel(), -1);
}

// RED IF: micCaptureDrops_ is ignored. A drop splices sample N onto N+k, which
// through the Hann window is broadband energy in every bin -- the measurement is
// worthless and must not be reported as a measurement. F20.
TEST (SoundcheckController, CaptureDropAborts)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));

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
}

// RED IF: the sample rate recorded at Preflight is not re-checked. A rate change
// invalidates T, the reference spectrum X and every bin-to-Hz mapping at once,
// and getLastDeviceError() reports NOTHING in that case. inv 20, F15.
TEST (SoundcheckController, SampleRateChangeAborts)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    auto p = r.params(); p.sampleRate = 44100.0;       // NOT the engine's rate
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), p));

    r.pump (50.0);
    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "device_changed");
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
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
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), p));

    r.pump (50.0);
    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "device_changed");
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: the hot-mic abort is dropped or its hold time is not enforced. A
// single over-threshold block must NOT abort; kMicAbortHoldMs of them must.
// Q3.
TEST (SoundcheckController, HotMicAbortsOnlyAfterTheHold)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
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
}

// RED IF: two abort reasons collapse onto one string. The log is the only place
// a soundman can find out why a run stopped, and "device_changed" and
// "capture_drop" ask for two different things to be checked. I-7.
TEST (SoundcheckController, EveryAbortReasonHasItsOwnName)
{
    using A = SoundcheckController::AbortReason;
    const A all[] { A::UserStop, A::Esc, A::EngineStopped, A::DeviceError,
                    A::DeviceChanged, A::MicHot, A::RoomRinging, A::CaptureDrop };

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
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
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
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    ASSERT_TRUE (r.pumpUntil (SoundcheckController::State::Results, 15000.0));

    EXPECT_EQ ((int) r.sc.copyResults().size(), 2);
    EXPECT_EQ ((int) r.sc.copyResultsForSlot (0).size(), 2);
    EXPECT_EQ ((int) r.sc.copyResultsForSlot (1).size(), 0);
    for (const auto& res : r.sc.copyResultsForSlot (0))
        EXPECT_EQ (res.slot, 0);
}
