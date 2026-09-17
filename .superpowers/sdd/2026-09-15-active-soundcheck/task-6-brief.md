### Task 6: `SoundcheckController` — the state machine, the refusals, the aborts

**Mức level dự kiến (spec §3):** this task is what makes Task 5's engine actually emit. Per output channel, in order: **0.5 s silence** (noise floor), **30 ms raised-cosine ramp in**, **3.0 s sweep at −20 dBFS peak / ≈ −23 dBFS RMS**, **0.7 s silence** (tail), **0.3 s silence** (gap, lanes already restored) — **4.5 s per channel, ~72 s worst case**. Every other output channel: **0 dB**. On `Results`: **0 dB, and detection is already back on**, so lane G's release ladder is running normally. Nothing is placed: **0 dB** until `ÁP DỤNG` in Task 7.

**The machine** (spec §4.3), transcribed so no implementer has to re-read the spec:

```
Idle
 └─(request)→ Preflight   check the refusals; RECORD sampleRate + in/out channel counts
      └─→ Confirm         dialog: "HẠ MASTER TRƯỚC" + the total duration (~72 s worst case)
           └─(OK)→ Arm    CHECK RING RISK ONE LAST TIME (the snapshot is still live here);
                          READ the noise-floor gate -> RunParams.noiseFloorGate;
                          scSuspendTaps_ = true  (HELD to the end of the LAST channel);
                          detection OFF on every slot; lock the controls (§4.9);
                          log soundcheck_start
                └─→ [for each output channel]
                     NoiseFloor 0.5 s  scOutChannel_ set (already muted),
                                       scCaptureActive_ = true,
                                       scSampleIndex_ = -noiseFloorSamples
                       └─ GATE: max peakinessAt(noise window) >= RunParams::noiseFloorGate
                                ⇒ Abort(room_ringing)
                     Sweep 3.0 s       scSampleIndex_ passes through 0
                     Tail 0.7 s
                     Analyse           compute H, pick candidates; log soundcheck_output
                     Gap 0.3 s         scOutChannel_ = -1, scCaptureActive_ = false
                                       scSuspendTaps_ STAYS true
                └─(channels exhausted)→ Restore-detection  scSuspendTaps_ = false;
                                                           detection back ON immediately
                     └─→ Results       log soundcheck_result; unlock everything but PRESET LOAD
Results  (detection already ON, taps running again, snapshot live again)
 ├─(ÁP DỤNG)→ Apply  (message thread) → log soundcheck_apply → Idle
 ├─(BỎ)      → Idle
 └─(kResultsTimeoutMs = 20 s elapses)→ Idle

Abort  ← from any emitting phase: set scRampOutAtSample_ (the callback does the rest),
       wait for scOutChannel_ to reach -1, scSuspendTaps_ = false, detection back on,
       unlock, log soundcheck_abort → Idle
```

**Four things this machine gets right that spec rev 1 got wrong, each of which an implementer will be tempted to "simplify" back:**

1. **`Results` runs WITH detection back on** (F9). Rev 1 held detection off for 60 s to avoid racing for an `index`. But the taps resume the moment suspension lifts, so `liveMs_` advances (`src/app/NotchController.cpp:662-664`), `riskValid` is false so `frozen` is false, and the release ladder walks 60 s: a −24 notch loses its first rung at 30 s and its second at 40 s — while spec §3 declares 0 dB. Detection comes back **the instant the last channel's tail ends**, and `kResultsTimeoutMs` is 20 s regardless.
2. **The `index` race is handled by re-reading, not by disarming the detector** (F10) — Task 7.
3. **Ring risk is read only where it is still alive.** Making `ringRiskScore >= ringRiskThreshold` a *mid-run* abort condition **can never fire**: detection is off, so `processSpectrumForDetection` returns immediately (`src/app/NotchController.cpp:1281-1282`) and `frameScoreValid_` is never true; and the taps are suspended, so no block is drained and the snapshot never refreshes (`:617-650`). Ring risk is therefore read at **`Preflight` and again at `Arm`**, while the taps still run. The "is the room ringing?" question *during* a run is answered by lane M's own measurement instead: the 0.5 s noise window goes through `PeakinessAnalyzer::peakinessAt` and the **peakiest bin** is compared against `RunParams::noiseFloorGate`.
4. **The mic level for self-abort is computed on the lane M thread** from `micCapture_` (20 ms sliding RMS), never taken from anything of the detector's.

**`RunParams` — read ONCE at `Arm` on the message thread, then immutable.** Every field being frozen is the point: a threshold that moved mid-run would score channel 1 and channel 9 of the *same measurement* on two different rulers, and nobody reading the log could tell.

**Files:**
- Create: `src/app/SoundcheckController.h`, `src/app/SoundcheckController.cpp`
- Create: `tests/test_soundcheckcontroller.cpp`
- Modify: **`tests/test_notchcontroller.cpp`** — N-4. `APreventiveNotchNeitherWritesNorConsumesRoomMemory` lands **here**, not in the new file, because it needs `Harness`, `NoiseSource` and `probeMemoryAt` (`tests/test_notchcontroller.cpp:20`, `:335`, `:400`), all of which live in that TU's anonymous namespace. Append it after the last anonymous namespace closes (lane G m-E)
- Modify: `CMakeLists.txt` (`HANDSFREE_CORE_SOURCES`), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `AudioEngine` setters and `getMicCaptureBuffer` / `getMicCaptureDropCount` (Task 5); `SoundcheckSignal` (1), `LoopGainEstimator` (2), `SoundcheckCandidates` (3); `ClockSource` (`src/dsp/ClockSource.h:8-13`); `PeakinessAnalyzer::peakinessAt` (`src/dsp/PeakinessAnalyzer.h:166`); `NotchController::SnapshotBuffer` and the three constants `kRiskFreezeFraction` / `kDepthLadderDb` / `kMaxDepthDb` **as values, never through a stored pointer** (inv 17).
- Produces:
  ```cpp
  class SoundcheckController : private juce::Thread
  {
  public:
      static constexpr double kTailSeconds      = 0.7;
      static constexpr double kGapMs            = 300.0;
      static constexpr double kNoiseFloorMs     = 500.0;
      static constexpr double kMicAbortDbfs     = -6.0;
      static constexpr double kMicAbortHoldMs   = 20.0;
      static constexpr double kResultsTimeoutMs = 20000.0;
      static constexpr double kPollMs           = 5.0;
      // Aliases -- one definition each, no second literal (lane G m-D):
      static constexpr double kSweepLowHz        = SoundcheckSignal::kSweepLowHz;
      static constexpr double kSweepHighHz       = SoundcheckSignal::kSweepHighHz;
      static constexpr double kSweepSeconds      = SoundcheckSignal::kSweepSeconds;
      static constexpr double kRampMs            = SoundcheckSignal::kRampMs;
      static constexpr double kRampOutMs         = SoundcheckSignal::kRampOutMs;
      static constexpr float  kSoundcheckMaxPeak = SoundcheckSignal::kSoundcheckMaxPeak;
      static constexpr float  kSoundcheckMinPeak = SoundcheckSignal::kSoundcheckMinPeak;
      static constexpr double kTrustedHighHz     = LoopGainEstimator::kTrustedHighHz;
      static constexpr double kMinBandSnrDb      = LoopGainEstimator::kMinBandSnrDb;
      static constexpr double kMinBinSnrDb       = LoopGainEstimator::kMinBinSnrDb;
      static constexpr double kCandidateMarginDb = SoundcheckCandidates::kCandidateMarginDb;
      static constexpr double kMinUsefulCutDb    = SoundcheckCandidates::kMinUsefulCutDb;
      static constexpr double kMinProminenceDb   = SoundcheckCandidates::kMinProminenceDb;
      static constexpr double kTargetMarginDb    = SoundcheckCandidates::kTargetMarginDb;
      static constexpr int    kMaxPreventivePerLane = SoundcheckCandidates::kMaxPreventivePerLane;

      enum class State  { Idle, Preflight, Confirm, Arm, NoiseFloor, Sweep, Tail,
                          Analyse, Gap, Results, Abort };
      enum class Refusal { None, EngineNotRunning, NoChannels, SlotDisabled,
                           InvalidChannelPair, RingRiskRising };
      enum class AbortReason { UserStop, Esc, EngineStopped, DeviceError, DeviceChanged,
                               MicHot, RoomRinging, CaptureDrop };

      struct Target { int slot = 0, lane = 0, outChannel = 0, inChannel = 0; };

      struct RunParams
      {
          float  noiseFloorGate   = 0.0f;   // = getPeakinessThreshold() at Arm. A PEAKINESS
                                            // RATIO, never a 0..1 score (N1).
          float  peak             = 0.0f;   // already clamped <= kSoundcheckMaxPeak
          double sampleRate       = 0.0;
          int    numInputChannels = 0, numOutputChannels = 0;
          double ceilingDb        = 0.0;
          double notchQ           = 0.0;
      };

      struct OutputResult
      {
          int   slot = 0, lane = 0, outChannel = 0, inChannel = 0;
          bool  measured = false;         // false = "could not measure" (Q8) -- a VALID result
          bool  routingInvalid = false;   // different from measured == false (F26)
          float snrDb = 0.0f;
          std::array<float, LoopGainEstimator::kNumBins> marginDb {};   // = -H_dB
          std::array<bool,  LoopGainEstimator::kNumBins> trusted {};
          // I-3: the PER-BIN flags, not just the count. SpectrumView's overlay
          // draws a marker per marked bin, and SoundcheckCandidates::Output
          // already produces the array -- rev 1 dropped it on the way out and
          // left the GUI with no way to know WHICH bins were marked.
          std::array<bool,  LoopGainEstimator::kNumBins> marked {};
          int   markedCount = 0, candidateCount = 0, saturatedBins = 0;
          struct Candidate { float hz = 0.0f, marginDb = 0.0f, depthDb = 0.0f, q = 0.0f,
                             residualDb = 0.0f; int bin = 0; };
          std::array<Candidate, kMaxPreventivePerLane> candidates {};
      };

      SoundcheckController (AudioEngine& engine, ClockSource& clock);
      ~SoundcheckController() override;

      // --- MESSAGE THREAD ---
      [[nodiscard]] Refusal preflight (const std::vector<Target>& targets,
                                       const NotchController::SnapshotBuffer& risk) const;
      bool  arm (std::vector<Target> targets, const RunParams& params);
      void  applyRequested();          // Results -> Idle, after Task 7 has placed
      void  dismissRequested();        // BO
      void  requestStop (AbortReason reason);
      void  abortAndJoin();            // device restart path; blocks until Idle

      // --- ANY THREAD (reads) ---
      [[nodiscard]] State  getState() const;
      [[nodiscard]] int    getCurrentTargetIndex() const;
      [[nodiscard]] int    getTargetCount() const;
      [[nodiscard]] double getElapsedMsInRun() const;
      [[nodiscard]] double getRemainingMsInRun() const;
      [[nodiscard]] std::vector<OutputResult> copyResults() const;
      // I-4: consumed by MainComponent's APPLY lambda (Task 10), which walks the
      // slots and calls applySoundcheckResults once per slot's NotchController.
      [[nodiscard]] std::vector<OutputResult> copyResultsForSlot (int slot) const;

      // Injected so this class holds NO NotchController pointer (inv 17). The
      // owner's lambda does nothing but relaxed atomic stores
      // (NotchController::setDetectionActive, NotchController.cpp:936-939).
      //
      // *** LIFETIME CONTRACT (I-10) ***
      // All three are ASSIGNED ONCE, BEFORE start(), AND NEVER AFTER. They are
      // invoked from the lane M thread; a std::function assigned while it is
      // being invoked is a data race, and these are bare public members with no
      // lock. Before any reassignment or destruction, abortAndJoin() or
      // stop() must have RETURNED. MainComponent assigns them in its
      // constructor (Task 10 Step 2), before the controller is ever started.
      std::function<void (bool)>          setDetectionActiveOnAllSlots;
      std::function<void (const juce::var&)> logEvent;      // message- or lane-M thread
      std::function<void()>               onStateChanged;   // GUI repaint request

      // One poll step. run() is this in a loop with wait(kPollMs); every test
      // drives it directly, so no test starts a thread. Same shape as
      // NotchController::runOnce (NotchController.h:349).
      void runOnce();

      void start();
      void stop (int timeoutMs);

      // TEST ACCESSORS ONLY.
      [[nodiscard]] float worstPeakinessForTest() const;   // last noise window's max
      // I-7: the enum -> string mapper, so a test can loop the ENUMERATORS
      // instead of asserting that eight string literals differ.
      [[nodiscard]] static const char* abortReasonNameForTest (AbortReason r);
      // I-8: forwards to the file-local round3sf so the log-shape test can call
      // the same rounding the writer uses, qualified.
      [[nodiscard]] static double roundToThreeSignificantFiguresForTest (double v);

  private:
      void run() override;
  };

  // MESSAGE THREAD ONLY. Task 7.
  struct SoundcheckApplyStats { int placed = 0, refused = 0, clearedPrevious = 0; };
  SoundcheckApplyStats applySoundcheckResults (
      NotchController& controller,
      const std::vector<SoundcheckController::OutputResult>& results);
  ```

**Why `setDetectionActiveOnAllSlots` is a `std::function` and not a `NotchController*`.** Invariant 17 says this class never calls `NotchController`, and invariant 12 says detection must be back on **before** `Results` is entered — which rules out a `MessageManager::callAsync` hop, because that returns immediately and the state machine would reach `Results` first. So the controller calls an injected lambda synchronously on its own thread, and `MainComponent`'s lambda is exactly:

```cpp
    soundcheck_.setDetectionActiveOnAllSlots = [this] (bool on)
    {
        // Relaxed atomic store per slot and nothing else
        // (NotchController::setDetectionActive, NotchController.cpp:936-939).
        // Safe from the lane M thread; this is the ONLY NotchController call
        // anywhere on that thread's stack, and it is here in MainComponent, not
        // in SoundcheckController, which holds no pointer at all (inv 17).
        for (auto& c : notchControllers_)
            c->setDetectionActive (on);
    };
```

**This is the single place where the plan interprets the spec rather than transcribing it, and it is the first thing a reviewer should challenge.** The alternatives were: (a) hold a `NotchController*` — breaks inv 17's letter; (b) hop to the message thread — breaks inv 12's ordering; (c) have the GUI timer service a flag — same ordering problem. Task 10 adds `RestoreDetectionCallbackOnlyTouchesAtomics`, which asserts the lambda's whole observable effect is `detectionActiveForTest()` (`src/app/NotchController.h:404-405`) on every slot.

**Timing.** Every phase deadline is `clock_.nowMs() + duration`; `runOnce()` compares against `clock_.nowMs()`. Sample counts are derived from `RunParams::sampleRate`, never from a live engine read, so a rate change is *detected* (and aborts) rather than silently changing the maths.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_soundcheckcontroller.cpp`. Fixture first:

```cpp
// tests/test_soundcheckcontroller.cpp
//
// Fake clock, real AudioEngine (no device -- the callback is public and driven
// by hand, exactly as tests/test_audioengine.cpp does it), no thread: every test
// calls runOnce() itself.
#include <gtest/gtest.h>

#include "app/AudioEngine.h"
#include "app/NotchController.h"
#include "app/SoundcheckController.h"

#include <cmath>
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
        // audioDeviceAboutToStart() does not touch it (:686-739), so plan rev 1's
        // `engine.audioDeviceAboutToStart (nullptr)` left preflight() returning
        // EngineNotRunning and every armed run aborting engine_stopped.
        setRunning (true);
        // numInputChannels_/numOutputChannels_ are written ONLY from the
        // callback (:506-507; tests/test_audioengine.cpp:409-410 already relies
        // on this), so preflight's channel-count check needs one real block to
        // have flowed first.
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
```

Now the refusal tests. **Each asserts that NOT ONE SAMPLE was emitted and the state is back to `Idle`** (inv 19):

```cpp
// RED IF: Preflight stops checking isRunning(). AudioEngine's channel counts are
// written only from the callback (AudioEngine.cpp:506-507) and are never reset
// on stop, so a non-zero count can be a leftover from the previous device
// session -- checking the counts alone is not enough. F26.
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
    // RunParams, so rev 1's local `p` here was dead code. An empty target list is
    // what "nothing to measure" actually looks like.
    EXPECT_EQ (r.sc.preflight ({}, risk (false, 0.0f)),
               SoundcheckController::Refusal::NoChannels);
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: a disabled slot is measured anyway. Its lanes are not in the callback's
// lane table at all (AudioEngine.cpp:530-531), so the sweep would go out with
// nothing to compare it against.
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
// measure". The lane loop merely `continue`s past a bad pair
// (AudioEngine.cpp:546-548), so without this check bad routing reads as a quiet
// room instead of as a routing fault. F26.
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

    // ...and the third case is RECORDED, not silently dropped.
    ASSERT_TRUE (r.sc.arm (targets, r.params()));
    r.pump (20.0);
    bool sawNullRisk = false;
    for (const auto& v : r.log)
        if (evOf (v) == "soundcheck_start")
            sawNullRisk = v.getDynamicObject()->getProperty ("ring_risk").isVoid();
    EXPECT_TRUE (sawNullRisk) << "ring_risk must be null, not 0.0";
}
```

Then the run-shape and abort tests:

```cpp
// RED IF: the channels are measured simultaneously instead of one at a time.
// Two sweeps at once measures neither loop. inv 5.
TEST (SoundcheckController, SequencesOneOutputChannelAtATime)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
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
}

// RED IF: kResultsTimeoutMs drifts back up. 20 s is the number lane G's release
// ladder can tolerate without losing a rung. F9.
TEST (SoundcheckController, ResultsTimeoutIsTwentySeconds)
{
    EXPECT_DOUBLE_EQ (SoundcheckController::kResultsTimeoutMs, 20000.0);

    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params()));
    // I-1: the run itself is 2 x 4.5 s = 9000 ms, so pump(12000) is already
    // 3000 ms INTO Results. Rev 1 then pumped another 19000, reaching 22000 ms >
    // kResultsTimeoutMs, and asserted Results on a state machine that had
    // correctly gone Idle. Measure from the moment Results is entered.
    r.pump (12000.0);
    ASSERT_EQ (r.sc.getState(), SoundcheckController::State::Results);

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

// RED IF: the noise-floor gate is compared against kConfirmScore = 0.7 -- a
// 0..1 PRODUCT -- instead of against the live PEAKINESS RATIO. Every real room
// reads ~7 on a quiet noise floor, so that mistake aborts every run everywhere.
// This test going green is the only thing that catches it. N1.
TEST (SoundcheckController, NoiseFloorOfAQuietRoomDoesNotAbort)
{
    Rig r;
    r.micSource = whiteNoise (16384, 1.0e-3f);

    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (10.0f)));
    r.pump (2000.0);

    // FLAKE GUARD, and it must come FIRST.
    //
    // The 7.35 figure (PeakinessAnalyzer.h:60-65) is a 60-seed ONE-SHOT maximum.
    // Dense continuous sampling crossed 10.0 once, at 13.99
    // (memory/peakiness-sweep-2048-2026-09-04.md). This fixture takes the max
    // over ~1023 bins of a looped 16384-sample buffer, which will eventually
    // exceed a gate of 10 for reasons that have nothing to do with the code
    // under test. Pinning the fixture first means a drifting fixture fails AS a
    // fixture problem, with a message that says so, instead of masquerading as a
    // broken gate.
    ASSERT_LT (r.sc.worstPeakinessForTest(), 10.0f)
        << "the synthetic noise floor drifted over the gate -- reseed the fixture, "
           "do not relax the gate";

    EXPECT_NE (r.sc.getState(), SoundcheckController::State::Idle);
    EXPECT_FALSE (sawEvent (r.log, "soundcheck_abort"));
}

// RED IF: the gate is removed, or raised so far that a genuinely ringing room
// gets swept anyway. N1.
TEST (SoundcheckController, NoiseFloorWithARingingToneAborts)
{
    Rig r;
    r.micSource = noisePlusTone (16384, 1.0e-3f, 1000.0, 0.05f);

    ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (10.0f)));
    r.pump (2000.0);

    EXPECT_TRUE (sawEvent (r.log, "soundcheck_abort"));
    EXPECT_EQ (abortReasonInLog (r.log), "room_ringing");
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
            // A tone whose peakiness lands BETWEEN the two gates.
        r.micSource = noisePlusTone (16384, 1.0e-3f, 1000.0, 0.004f);

        ASSERT_TRUE (r.sc.arm (r.stereoTargets(), r.params (gate)));
        r.pump (2000.0);

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

// RED IF: the abort path waits for the controller thread. The thread is
// deliberately not polled after the request; only the callback runs. inv 9, F8.
TEST (SoundcheckController, AbortRampsDownInTheCallbackAlone)
{
    Rig r;
    r.micSource = whiteNoise (4096, 1.0e-4f);
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
    const juce::AudioIODeviceCallbackContext ctx {};
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
    EXPECT_EQ (r.sc.getState(), SoundcheckController::State::Idle);
}

// RED IF: the hot-mic abort is dropped or its hold time is not enforced. A
// single over-threshold sample must NOT abort; kMicAbortHoldMs of them must.
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
}

// RED IF: lane G's room memory is touched by a preventive notch.
//
// I-6, and this is why the test moved into tests/test_notchcontroller.cpp and
// changed shape entirely. BOTH Soundcheck room-memory lines live inside
// placeConfirmed:
//
//   NotchController.cpp:1116 -- if (index >= 0 && origin != Origin::Soundcheck)
//                               remembered = takeRememberedDepthLocked(...)
//                               i.e. a Soundcheck placement never CONSUMES an entry
//   NotchController.cpp:1169 -- if (origin == Origin::Soundcheck) depthDb = ceiling;
//                               i.e. a remembered depth never DECIDES a Soundcheck depth
//
// Neither is reachable from setNotch/clearNotch, and roomMemory_ is written only
// on the auto-release path (:360-405). Plan rev 1's version called setNotch and
// clearNotch only, so it exercised nothing and could not go red no matter what
// lane M did. This version drives a REAL detector placement through the
// probeMemoryAt family (tests/test_notchcontroller.cpp:400), which is the only
// route into placeConfirmed the headless suite has. inv 18, F20.
TEST (NotchControllerSoundcheck, APreventiveNotchNeitherWritesNorConsumesRoomMemory)
{
    Harness h;
    NoiseSource quiet;

    // 1. A real detector howl at 1 kHz, driven to a depth, then auto-released --
    //    THIS is what writes a room-memory entry (NotchController.cpp:360-405).
    //    probeMemoryAt returns the depth a fresh howl at `freq` is placed at, so
    //    a remembered entry shows up as a depth deeper than the first rung.
    const double remembered = probeMemoryAt (h, quiet, 1000.0);
    ASSERT_LT (remembered, -6.0) << "the fixture never built a memory entry to protect";

    // 2. A preventive notch at the SAME bin, then cleared the way a re-run clears
    //    it. If lane M consumed the entry, step 3 loses its memory.
    ASSERT_TRUE (h.controller.setNotch (0, 15, 1000.0, 30.0, -24.0,
                                        NotchController::Origin::Soundcheck));
    h.controller.clearNotch (0, 15, NotchController::ClearReason::SoundcheckReplace);
    const std::vector<float> block (512, 0.0f);
    h.tap.write (block.data(), block.size());
    h.controller.runOnce();

    // 3. The entry is still there: a fresh detector howl at that bin is still
    //    placed at the remembered depth, not crawling up from -6.
    EXPECT_DOUBLE_EQ (probeMemoryAt (h, quiet, 1000.0), remembered);
}
```

- [ ] **Step 2: Run and watch them fail to compile**

```bash
cmake --build build --config Release
```
Expected: `Cannot open include file: 'app/SoundcheckController.h'`.

- [ ] **Step 3: Write `src/app/SoundcheckController.h`**

The declaration is in the Interfaces block above. Add a header comment that states, in this order: the thread map (message thread / lane M thread / audio callback); that this class holds **no** `NotchController` pointer and why; that `RunParams` is frozen for the run and why; and that `runOnce()` is the whole state machine and `run()` is only a loop around it.

Private state:

```cpp
private:
    void run() override;

    // --- phase helpers, all on the lane M thread ---
    void enterTarget (int index);
    void enterGap();
    void finishRun();
    void beginAbort (AbortReason reason);
    bool checkDeviceUnchanged();          // sample rate + channel counts vs RunParams
    bool checkMicNotHot (const float* block, int n);
    bool noiseWindowIsRinging() const;    // peakinessAt vs params_.noiseFloorGate
    void drainCapture();
    void analyseCurrentTarget();

    AudioEngine& engine_;
    ClockSource& clock_;

    mutable std::mutex stateMutex_;       // guards state_, results_, targets_
    std::atomic<State> state_ { State::Idle };
    std::vector<Target>       targets_;
    std::vector<OutputResult> results_;
    RunParams params_ {};

    int    targetIndex_    = 0;
    double phaseEndsAtMs_  = 0.0;
    double runStartedAtMs_ = 0.0;
    double micHotSinceMs_  = -1.0;
    std::uint64_t dropsAtArm_ = 0;
    std::atomic<bool> stopRequested_ { false };
    std::atomic<AbortReason> stopReason_ { AbortReason::UserStop };

    LoopGainEstimator estimator_ { 48000.0 };
    std::vector<float> capture_;          // drain scratch, sized once at Arm
    std::vector<float> noiseWindow_;      // last kNoiseFloorMs of capture
    std::vector<float> reference_;        // the regenerated sweep, built once at Arm
    // The magnitude spectrum of the noise window, so noiseWindowIsRinging() can
    // run PeakinessAnalyzer::peakinessAt over it without allocating.
    std::array<float, LoopGainEstimator::kNumBins> noiseMagnitudes_ {};
    // The worst peakiness the last noise window produced. Published through
    // worstPeakinessForTest() so a fixture that drifts over the gate fails as a
    // FIXTURE problem rather than as a false gate failure (cross-check "Also").
    std::atomic<float> worstPeakiness_ { 0.0f };
```

`capture_`, `noiseWindow_` and `reference_` are sized in `arm()` on the message thread and never resized afterwards — the lane M thread allocates nothing per poll.

- [ ] **Step 4: Write `src/app/SoundcheckController.cpp`**

`preflight` in full — it is short, and it is the only place three of the refusals exist:

```cpp
SoundcheckController::Refusal
SoundcheckController::preflight (const std::vector<Target>& targets,
                                 const NotchController::SnapshotBuffer& riskSnapshot) const
{
    if (! engine_.isRunning())
        return Refusal::EngineNotRunning;

    const int ins  = engine_.getNumInputChannels();
    const int outs = engine_.getNumOutputChannels();
    if (ins <= 0 || outs <= 0 || targets.empty())
        return Refusal::NoChannels;

    for (const auto& t : targets)
    {
        const auto cfg = engine_.getSlotConfig (t.slot);
        if (! cfg.enabled || (cfg.width != 1 && cfg.width != 2))
            return Refusal::SlotDisabled;

        // F26: the lane loop merely `continue`s past a bad pair
        // (AudioEngine.cpp:546-548), so a routing fault would otherwise read as
        // "could not measure" -- a very different message for the operator.
        if (t.lane < 0 || t.lane >= cfg.width
            || t.inChannel  < 0 || t.inChannel  >= ins
            || t.outChannel < 0 || t.outChannel >= outs
            || cfg.inputChannels[t.lane]  != t.inChannel
            || cfg.outputChannels[t.lane] != t.outChannel)
            return Refusal::InvalidChannelPair;
    }

    // R3-2. BOTH sides are 0..1 products here: ringRiskScore is the same `score`
    // the placement decision compares against kConfirmScore, and
    // ringRiskThreshold IS kConfirmScore (NotchController.cpp:648). The product
    // is taken from the snapshot rather than written as 0.385 so the GUI's
    // RISING band and this gate cannot drift apart.
    //
    // ringRiskValid == false does NOT refuse: it means "no frame scored yet",
    // which is true in Bypass and after every reset. It is logged as null.
    if (riskSnapshot.ringRiskValid
        && riskSnapshot.ringRiskScore
             >= NotchController::kRiskFreezeFraction * riskSnapshot.ringRiskThreshold)
        return Refusal::RingRiskRising;

    return Refusal::None;
}
```

`arm()` on the message thread: store `params_` and `targets_`, size the three buffers, build `reference_` from a `SoundcheckSignal` with `params_`, record `dropsAtArm_ = engine_.getMicCaptureDropCount()`, call `engine_.setSoundcheckPeak (params_.peak)`, `engine_.setSoundcheckTapsSuspended (true)`, `setDetectionActiveOnAllSlots (false)`, log `soundcheck_start`, then `enterTarget (0)`.

`runOnce()` is a switch on `state_`. Every emitting state, **first**: `drainCapture()`, then `checkDeviceUnchanged()`, `engine_.getMicCaptureDropCount() != dropsAtArm_`, `engine_.isRunning()`, `engine_.getLastDeviceError().isNotEmpty()`, and `checkMicNotHot(...)` — any failure calls `beginAbort(...)`. Then the phase deadline.

`noiseWindowIsRinging()`:

```cpp
bool SoundcheckController::noiseWindowIsRinging() const
{
    // Both sides are PEAKINESS RATIOS, unbounded above: peakinessAt returns a
    // ratio (measured on the rig: worst noise bin 7.35, a 1 kHz tone 131.70 --
    // PeakinessAnalyzer.h:60-65), and params_.noiseFloorGate is the detector's
    // OWN live peakiness threshold, read at Arm. It is NEVER kConfirmScore,
    // which is the threshold of a 0..1 product -- that comparison aborts every
    // run in every room (N1, and memory/ring-risk-lane-r-2026-09-06.md).
    //
    // analyse() hides the distribution below its own threshold, so the maximum
    // is taken directly from peakinessAt, bin by bin
    // (memory/peakiness-sweep-2048-2026-09-04.md).
    float worst = 0.0f;
    for (int k = 1; k < LoopGainEstimator::kNumBins - 1; ++k)
        worst = std::max (worst, PeakinessAnalyzer::peakinessAt (
                                     noiseMagnitudes_.data(), LoopGainEstimator::kNumBins, k));
    return worst >= params_.noiseFloorGate;
}
```

`analyseCurrentTarget()` fills the `OutputResult` for the channel just measured, and the copy is **field by field and explicit** (m-22: `SoundcheckCandidates::Candidate` and `OutputResult::Candidate` are two identical-looking types in two layers, and nothing converts them automatically):

```cpp
    const auto est  = estimator_.finish();
    const auto pick = SoundcheckCandidates::pick (buildPickInput (est));

    OutputResult r;
    r.slot = t.slot; r.lane = t.lane; r.outChannel = t.outChannel; r.inChannel = t.inChannel;
    r.measured = est.measured;
    r.snrDb    = est.bandSnrDb;
    r.trusted  = est.trusted;
    r.marked   = pick.marked;                       // I-3: the PER-BIN flags
    for (int k = 0; k < LoopGainEstimator::kNumBins; ++k)
        r.marginDb[(std::size_t) k] = -est.hDb[(std::size_t) k];   // margin == -H_dB, ONE place
    r.markedCount = pick.markedCount;
    r.candidateCount = pick.candidateCount;
    r.saturatedBins  = pick.saturatedBins;
    for (int c = 0; c < pick.candidateCount; ++c)
    {
        const auto& src = pick.candidates[(std::size_t) c];
        auto&       dst = r.candidates[(std::size_t) c];
        dst.hz = src.hz; dst.marginDb = src.marginDb; dst.depthDb = src.depthDb;
        dst.q = src.q; dst.residualDb = src.residualDb; dst.bin = src.bin;
    }
```

`finishRun()`, in this exact order (inv 12): `engine_.setSoundcheckTapsSuspended (false)` → `setDetectionActiveOnAllSlots (true)` → log `soundcheck_result` → `state_ = State::Results` and stamp the 20 s deadline. **Detection must be restored before the state changes**, or a GUI polling `getState()` sees `Results` while the detector is still disarmed.

`beginAbort (reason)`: `engine_.requestSoundcheckRampOut()` **first** (the callback then owns the sound), then `engine_.setSoundcheckCaptureActive (false)`, `setSoundcheckTapsSuspended (false)`, `setDetectionActiveOnAllSlots (true)`, log `soundcheck_abort` with `reason` / `at_output` / `elapsed_ms`, `state_ = State::Idle`. It never waits for the ramp: the callback finishes it alone.

`run()`:

```cpp
void SoundcheckController::run()
{
    while (! threadShouldExit())
    {
        runOnce();
        wait ((int) kPollMs);
    }
}
```

`abortAndJoin()` (the device-restart path, Task 10): `requestStop (AbortReason::DeviceChanged)`, `runOnce()` once so the abort actually executes, then `stop (1000)`.

- [ ] **Step 5: Add to both CMake lists, reconfigure, build, run**

```cmake
    ${CMAKE_SOURCE_DIR}/src/app/SoundcheckController.cpp
    ${CMAKE_SOURCE_DIR}/src/app/SoundcheckController.h
```
```cmake
    test_soundcheckcontroller.cpp
```
```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```
```bash
cmake --build build --config Release
```
```bash
cd build && ctest -C Release -R SoundcheckController --output-on-failure
```
Expected: `100% tests passed` (**21** tests — m-14 recounted this; rev 1 said 20). Note that `APreventiveNotchNeitherWritesNorConsumesRoomMemory` lands in `tests/test_notchcontroller.cpp`, not in the new file, so `-R SoundcheckController` shows 20 and the full run shows 21.

**If `NoiseFloorGateIsReadAtArm`'s tone amplitude does not land between gate 5 and gate 20, derive it rather than nudging it.** `peakinessAt` is scale-invariant (`src/dsp/PeakinessAnalyzer.cpp:59-76`), so what decides the reading is the tone's bin magnitude **relative to its neighbours**, not its absolute level: put the ratio in the comment beside the number, as lane G's B-5 lesson requires (two lines of algebra beat a build).

- [ ] **Step 6: Full gate and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed (608)` — 587 + 21. ESTIMATE.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/SoundcheckController.h src/app/SoundcheckController.cpp tests/test_soundcheckcontroller.cpp tests/test_notchcontroller.cpp CMakeLists.txt tests/CMakeLists.txt
```
```bash
git commit -m "feat(lane-m): SoundcheckController -- state machine, refusals, self-aborts on a fake clock"
```

---

