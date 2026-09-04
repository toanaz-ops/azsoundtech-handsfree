// NotchController: the detector side of the audio<->detector bridge.
//
// Owns the AUTHORITATIVE notch model (owner decision D-05: the detector is
// the sole author of every Set/Clear) and is the sole producer of
// NotchCommands into the SPSC command ring that AudioEngine owns and drains.
//
// A sole author does not need to read back what it wrote -- it remembers what
// it commanded. The model is richer than NotchChain::NotchInfo because
// auto-release needs per-notch clocks (lockedAtMs, lastDetectedMs), which an
// audio-thread struct cannot carry.
//
// Threading (final form, arrived at over successive tasks):
//   - Policy entry points (setNotch/clearNotch/clearAll/adoptPreset):
//     message thread.
//   - run(): the detector thread loop -- runOnce() then wait(5). Polling,
//     never signalled from the audio thread: a WaitableEvent signal is a
//     kernel transition on a thread whose contract here is no locks/
//     allocation/logging; polling costs one wake per 5 ms against a ~170 ms
//     tap margin (design §4).
//   - Everything shared between those sides sits under modelMutex_. Neither
//     side is real-time, so an ordinary mutex is correct here; lock-free
//     machinery is reserved for the two channels touching the audio thread.
//
// Validation-before-send (bridge design §3, amended): the detector validates
// every command with the same predicates Biquad::setNotchFilter applies --
// sampleRate > 0, Q > 0, 0 < freq < sampleRate/2, depthDB <= 0 -- so the
// biquad's silent-rejection path is unreachable in practice.

#pragma once

#include "app/PresetManager.h"
#include "app/SlotConfig.h"
#include "dsp/CandidateScorer.h"
#include "dsp/ClockSource.h"
#include "dsp/Detector.h"
#include "dsp/LockFreeRingBuffer.h"
#include "dsp/NotchCommand.h"
#include "dsp/PeakinessAnalyzer.h"

#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

// private inheritance: the thread is an implementation detail; nothing
// external should see Thread's interface.
class NotchController : private juce::Thread
{
public:
    // Recorded because D-05's rejected alternative ("detector leaves preset
    // notches alone") becomes a one-line policy change if origin is kept,
    // and impossible to add later if it is not.
    // KD-7: Soundcheck-origin notches are EXEMPT from auto-release -- they
    // clear only via an explicit clearNotch/clearAll.
    enum class Origin { Detector, Preset, Manual, Soundcheck };

    static constexpr int kChannels   = 2;
    static constexpr int kSlots      = 16;
    static constexpr int kTotalSlots = kChannels * kSlots;
    static_assert (kChannels == kMaxSlotLanes, "lanes and channels are the same axis");

    // Auto-release measures real elapsed time but ONLY while the tap is
    // delivering audio (owner decision D-06). The gate is "the tap delivered
    // RECENTLY", not "it delivered this poll" -- see the design doc §4 for
    // why the naive form halves the clock rate. 250 ms exceeds the longest
    // legitimate gap between tap writes (2048 samples @ 44.1 kHz = 46.4 ms)
    // with >5x margin. Bound assumed: buffer sizes up to ~2730 @ 44.1 kHz.
    static constexpr double kTapSilenceTimeoutMs = 250.0;
    // Spec §5.2 step 7: 30 s without peakiness releases a notch.
    static constexpr double kAutoReleaseMs       = 30000.0;

    static constexpr double kSoundcheckDurationMs = 15000.0;
    // Spec 5.2 step 6: a candidate must persist this many consecutive blocks
    // before a Set is emitted (~30 ms at a 10.67 ms hop). Runtime-tunable
    // (brief 2026-08-24); kPersistenceBlocks is only the DEFAULT.
    static constexpr int kPersistenceBlocks       = 3;
    static constexpr int kMinPersistenceBlocks    = 1;
    static constexpr int kMaxPersistenceBlocks    = 10;
    // Automatic-notch defaults (KD-5), runtime-tunable per the brief. Q in
    // [8, 50], depth dB in [-24, -6]; clamped on set.
    static constexpr double kDefaultNotchQ      = 30.0;
    static constexpr double kDefaultNotchDepthDb = -18.0;   // was -12 pre-brief

    NotchController (LockFreeRingBuffer<float>& tapLane0,
                     LockFreeRingBuffer<float>* tapLane1,
                     LockFreeRingBuffer<NotchCommand>& commands,
                     ClockSource& clock,
                     int slotId = 0);
    // Legacy shape: one tap. Behaves LINKED whatever setLinked() says (S-6):
    // with no lane-1 spectrum there is nothing to be independent about.
    NotchController (LockFreeRingBuffer<float>& tap,
                     LockFreeRingBuffer<NotchCommand>& commands,
                     ClockSource& clock,
                     int slotId = 0);

    bool hasLaneOneTapForTest() const { return taps_[1] != nullptr; }   // TEST ACCESSOR ONLY

    ~NotchController() override;

    // Lifecycle. start() launches the poll loop; stop(timeoutMs) joins the
    // thread. MainComponent MUST stop() this before any device restart can
    // clear the rings (bridge design §6.5 -- clear()'s precondition).
    void start();
    void stop (int timeoutMs);

    // Routing-slot width: how many lanes (channels 0..width-1) THIS slot's
    // controller drives. Accepts 1 or 2; anything else clamps into [1, 2].
    // Default 2 (the legacy stereo behaviour). Message thread ONLY, and only
    // while the detector thread is STOPPED -- same precondition as the policy
    // entry points below; width_ is read unlocked by runOnce(), so writing it
    // while the thread runs is a data race. Narrowing (2 -> 1) queues a Clear
    // for every active notch on the lanes leaving the slot, so they don't
    // linger in the model or keep running on the audio thread's chain.
    void setWidth (int lanes);

    // LINK mode (design §4.3). INDEP is the default: a confirm on lane l
    // notches lane l ONLY, so a howl into the left mic no longer costs the
    // right side a filter it never needed. LINK restores the 1.0.4 fan-out:
    // any lane's confirm notches BOTH at one index. Atomic, so unlike
    // setWidth() this may be flipped while the detector thread runs; a flip
    // never touches notches already placed (design §4.3 -- CLEAR ALL is how
    // an operator asks for symmetry back).
    void setLinked (bool linked) { linked_.store (linked, std::memory_order_relaxed); }
    bool isLinked() const        { return linked_.load (std::memory_order_relaxed); }
    // LINKED behaviour is forced whenever independence is impossible (S-6):
    // one lane driven, or no lane-1 tap to be independent ABOUT.
    bool effectiveLinked() const { return isLinked() || width_ < 2 || taps_[1] == nullptr; }

    // Policy entry points. Message thread. setNotch validates BEFORE touching
    // anything; false means nothing changed anywhere.
    bool setNotch (int channel, int index,
                   double frequency, double Q, double depthDB,
                   Origin origin);
    void clearNotch (int channel, int index);
    void clearAll();

    // Owner decision D-05: a preset loaded mid-show is ADOPTED -- its notches
    // enter the model with Origin::Preset and auto-release treats them like
    // any other notch (30 s un-reinforced -> released). Each preset notch is
    // installed on ALL width_ lanes of this slot (design §2 sizes the command
    // burst as lanes x 16). Returns how many preset notches were adopted; a
    // notch whose parameters fail validation on a lane is skipped entirely.
    int adoptPreset (const std::vector<PresetNotch>& notches);

    // KD-9: detection gating (Bypass must never place notches). Snapshot
    // publication is NOT affected by this flag.
    void setDetectionActive (bool active);

    // Spec 5.3: soundcheck detects for 15 s of LIVE time and its notches do
    // not auto-release. Sets detection active for the duration.
    void startSoundcheck();
    bool soundcheckActive() const;
    double getSoundcheckRemainingMs() const;   // 0 when inactive

    // One synchronous pump step: drain the spectrum, advance the live clock,
    // apply auto-release, flush the outbox.
    void runOnce();

    // Detection tuning (brief 2026-08-24). Message thread; every value lives
    // in an atomic loaded relaxed by the detector thread, so these are safe
    // WHILE the controller runs -- unlike setWidth(). Each forwards to the
    // owning analyzer/scorer or to this controller's own notch defaults.
    void   setRiseReferenceMs (double ms);        // clamped 100..1000, -> scorer
    double getRiseReferenceMs() const;
    double getRiseReferenceMs (int lane) const;
    void   setPersistenceBlocks (int blocks);     // clamped 1..10
    int    getPersistenceBlocks() const;
    void   setNotchDefaults (double q, double depthDb);   // Q 8..50, depth -24..-6
    double getNotchQ() const;
    double getNotchDepthDb() const;
    void   setPeakinessThreshold (float t);       // clamped 5..20, -> analyzer
    float  getPeakinessThreshold() const;
    float  getPeakinessThreshold (int lane) const;

    // Lane asymmetry bonus (design §4.4). A howl is geometrically asymmetric
    // -- one loudspeaker into one mic -- while stereo programme material is
    // not, so a candidate that is peaky on THIS lane and flat on the other is
    // more likely feedback. 1.0 is deliberately the DEFAULT and the floor:
    // the number has not been swept against real-room data yet, so out of the
    // box this changes no placement decision at all.
    static constexpr float kMinLaneAsymmetryBonus = 1.0f;
    static constexpr float kMaxLaneAsymmetryBonus = 2.0f;
    void  setLaneAsymmetryBonus (float b);        // clamped 1..2
    float getLaneAsymmetryBonus() const;

    // TEST ACCESSOR ONLY: the multiplier §4.4 applies to a candidate at `bin`.
    static float asymmetryMultiplierForTest (const float* mine, const float* other, int bin, float bonus)
        { return asymmetryMultiplier (mine, other, bin, bonus); }

    // TEST ACCESSOR ONLY -- like Detector::getAnalysisWindowForTest().
    double liveMsForTest() const;

    // TEST ACCESSOR ONLY -- the detection gate is otherwise observable only
    // through a live spectrum; headless tests assert the disarm directly.
    bool detectionActiveForTest() const
        { return detectionActive_.load (std::memory_order_relaxed); }

    // One consistent frame for the GUI: the spectrum and the notch list are
    // captured under one lock at one instant (bridge design §5). Caller owns
    // the destination -- returning a container would allocate every paint.
    struct SnapshotNotch
    {
        float frequency = 0.0f;
        float Q         = 0.0f;
        float depthDB   = 0.0f;
        std::uint8_t channel = 0;
        std::uint8_t index   = 0;
    };

    struct SnapshotBuffer
    {
        std::array<std::array<float, Detector::kNumBins>, kChannels> magnitudes {};
        std::uint32_t magnitudeCount = 0;
        std::uint32_t laneCount = 1;
        bool          linked = false;
        double sampleRate = 0.0;
        std::array<SnapshotNotch, kTotalSlots> notches {};
        std::uint32_t notchCount = 0;
        std::uint64_t sequence = 0;   // increments on every published frame
    };

    void copySnapshot (SnapshotBuffer& destOwnedByCaller) const;

    // Commands that had to be retried because the command ring was full.
    // Sustained growth means the audio callback stopped draining -- a real
    // fault the UI should be able to surface.
    std::uint64_t retryCount() const;

    void setSampleRate (double sampleRate);

private:
    void run() override;

    struct ModelNotch
    {
        double frequency    = 0.0;
        double Q            = 0.0;
        double depthDB      = 0.0;
        double lockedAtMs   = 0.0;
        double lastDetectedMs = 0.0;
        Origin origin       = Origin::Detector;
        bool   active       = false;
    };

    static constexpr int slotOf (int channel, int index) { return channel * kSlots + index; }

    void flushOutbox();
    void pushClearLocked (int channel, int index);

    // `otherLaneMagnitudes` is the opposite lane's spectrum, at most one hop
    // apart from `block` (runOnce()'s per-lane drain invariant), or nullptr
    // when only one lane produced a block this iteration.
    void processSpectrumForDetection (int lane, const Detector::Spectrum& block,
                                      const float* otherLaneMagnitudes, double blockNowMs);

    static float asymmetryMultiplier (const float* mine, const float* other, int bin, float bonus);

    // Index search, both under modelMutex_ (the suffix is a promise, not a
    // decoration). INDEP asks only about the lane it is placing on; LINKED
    // must find an index free on EVERY driven lane (S-7) -- the pre-1.0.5
    // "look at lane 0 only" rule would silently overwrite a lane-1 notch that
    // INDEP had placed, with no Clear to tell the chain about it.
    int  firstFreeIndexLocked (int lane) const;
    int  firstFreeIndexAllLanesLocked() const;
    void placeConfirmed (int lane, const PeakinessAnalyzer::Candidate& cand, bool linkedNow);

    double remainingSoundcheckMs() const;

    LockFreeRingBuffer<NotchCommand>& commands_;
    ClockSource&                    clock_;

    // Routing slot this controller owns; stamped onto EVERY NotchCommand it
    // emits so the engine routes it to the right chain.
    const int slotId_;
    // Lanes driven by this controller (1 or 2). Written only via setWidth()
    // with the detector thread stopped (see its comment); read from both
    // threads afterwards, which is safe because the write happens-before the
    // thread start/restart.
    int width_ = 2;

    // Per-lane detection state: one Detector/analyzer/scorer/persistence set
    // per channel, so each lane's FFT and candidate history is independent.
    struct LaneAnalysis
    {
        Detector          detector { 48000.0 };
        PeakinessAnalyzer analyzer;
        CandidateScorer   scorer;
        std::array<std::uint32_t, Detector::kNumBins> persistence {};
        double previousBlockNowMs = 0.0;   // <= 0: no previous block yet
    };
    std::array<LockFreeRingBuffer<float>*, kChannels> taps_ {};   // [0] never null
    std::array<LaneAnalysis, kChannels> lanes_;

    // Lanes actually analysed this run: 2 only when stereo AND a lane-1 tap exists.
    int analysedLanes() const { return (width_ == 2 && taps_[1] != nullptr) ? 2 : 1; }

    std::atomic<bool> detectionActive_ { false };
    // INDEP by default (design §4.3). Atomic because setLinked() is allowed
    // while the detector thread runs, unlike width_.
    std::atomic<bool>  linked_ { false };
    std::atomic<float> laneAsymmetryBonus_ { kMinLaneAsymmetryBonus };
    // Runtime tuning state (brief 2026-08-24): message thread writes, the
    // detector thread loads relaxed inside processSpectrumForDetection().
    std::atomic<int>    persistenceBlocks_ { kPersistenceBlocks };
    std::atomic<double> notchQ_      { kDefaultNotchQ };
    std::atomic<double> notchDepthDb_ { kDefaultNotchDepthDb };
    // Atomic is belt-and-braces only: ALWAYS accessed under modelMutex_
    // together with liveMs_, which is what actually serialises it.
    std::atomic<double> soundcheckEndsAtLiveMs_ { -1.0 };

    mutable std::mutex modelMutex_;               // guards model_ and outbox_ (mutable: liveMsForTest() is const)
    std::array<ModelNotch, kTotalSlots> model_;
    std::vector<NotchCommand> outbox_;

    std::atomic<std::uint64_t> retryCount_ { 0 };

    // Live-clock state, all under modelMutex_. liveMs_ advances only while
    // the tap is alive; EVERY spec timer (30 ms candidate persistence,
    // 15 s soundcheck, 30 s auto-release) reads liveMs_, never the wall clock.
    double liveMs_     = 0.0;
    double lastPollMs_ = 0.0;
    // Sentinel far below any real time: "the tap has never delivered".
    double lastDataMs_ = -1.0e9;

    // One mutex, one struct (design §5): spectrum and notch list are always
    // paired to the same instant. mutable: copySnapshot is const.
    mutable std::mutex snapshotMutex_;
    SnapshotBuffer    latest_;
};
