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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
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

    // Lane D (data loop). Why a notch left the model -- the session log's
    // training label depends on it, so the two internal unwind sites MUST say
    // PartialApplyUnwind rather than hide behind the Manual default.
    enum class ClearReason : std::uint8_t
    {
        Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind
    };

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

    // === Lane G: the depth ladder (spec 4.9). Fixed for 1.2.0 and
    // deliberately NOT exposed on the GUI -- these are the numbers the
    // ladder's behaviour was reasoned about with, and a slider on any of them
    // turns every future bug report into "which value was it on?".
    //
    // A Detector notch only ever STANDS on a rung. Deeper == more negative.
    static constexpr double kDepthLadderDb[4] = { -6.0, -12.0, -18.0, -24.0 };
    static constexpr int    kDepthLadderSize  = 4;
    static constexpr double kDepthStepDb      = 6.0;
    // Invariant 1: no Set this controller emits may be deeper than this, for
    // ANY Origin (Q12). -24 dB already kills any howl this app can hear.
    static constexpr double kMaxDepthDb       = -24.0;
    // How long a rung must hold before the ladder buys the next one down (Q2).
    // Measured against liveMs_, which advances once per runOnce AFTER the
    // drain loop, so the gate cannot fire twice inside one drain.
    static constexpr double kDeepenAfterMs    = 300.0;
    // A candidate whose RAW rise ratio is at least this starts on the second
    // rung (Q6). Magnitudes are amplitudes, so 2.0 == +6 dB over the rise
    // window. rNorm cannot express this: it saturates at 1.5.
    static constexpr float  kSteepRiseRatio   = 2.0f;
    // Release ladder (Q3): the FIRST rung costs 30 s of quiet, every rung
    // after it 10 s. kAutoReleaseMs keeps its name and value as the first step.
    static constexpr double kReleaseFirstMs   = kAutoReleaseMs;
    static constexpr double kReleaseStepMs    = 10000.0;
    // "Room memory" (Q6/Q10): a howl returning to the SAME BIN within this
    // window restarts at the depth it needed last time.
    static constexpr double kMemoryTtlMs      = 300000.0;
    static constexpr int    kMemoryEntriesPerLane = 16;
    // Release-clock freeze (Q4). The SAME fraction the GUI's RISING band uses
    // -- one constant, not two that can drift apart. This IS the definition:
    // gui::SpectrumView::kRingRiskRisingFraction is changed in Task 7 to alias
    // this name, so there is no second 0.55f literal anywhere. Frozen at
    // score >= 0.55 x CandidateScorer::kConfirmScore = 0.385.
    //
    // Do not move this declaration into a .cpp or behind an accessor: the GUI
    // header includes app/NotchController.h and needs it as a constant
    // expression.
    static constexpr float  kRiskFreezeFraction = 0.55f;

    // Why a notch's depth moved. Lane D writes it into the session log as
    // `reason` on a notch_retune line.
    enum class RetuneReason : std::uint8_t { Deepen, Release, Reclamp, Ceiling };

    // Ladder arithmetic. Pure and static, so it is testable without a rig.
    //
    // Q13: the EFFECTIVE ladder is the fixed rungs SHALLOWER than the ceiling,
    // plus the ceiling itself as the last rung -- so ceiling -10 (the shipped
    // presets/Music.json) gives -6 -> -10, and ceiling -6 gives a one-rung
    // ladder that never deepens. The `ceilingRung` IS the ceiling value and
    // may be an odd number: it is the ONLY place a Detector notch stands off a
    // fixed rung. There is deliberately no quantisation helper -- v2 had one
    // (`ceilingRungDb`), and quantising -10 down to -6 made Music 4 dB
    // shallower than 1.1.3 with nothing saying so.
    //
    // nextDeeperRungDb: the shallowest fixed rung strictly deeper than
    //   `currentDb`, capped at `ceilingDb`. Fix-round 1 (review finding
    //   "Important 2"): AT the ceiling this returns `currentDb` unchanged;
    //   DEEPER than the ceiling (currentDb < ceilingDb -- a lowered slider
    //   left an existing notch past its new ceiling) this returns the
    //   ceiling itself, which is SHALLOWER than currentDb. A caller that only
    //   ever deepens must compare the result against currentDb before
    //   sending a command, or it will re-send a shallower depth as if it were
    //   a step down.
    // nextShallowerRungDb: the deepest fixed rung strictly shallower than
    //   `currentDb`, saturating at -6. Needs no ceiling -- a release always
    //   moves toward a fixed rung.
    static double nextDeeperRungDb (double currentDb, double ceilingDb);
    static double nextShallowerRungDb (double currentDb);

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
    // Lane D: stop() ALSO flushes the event outbox after the join (or
    // immediately, if the thread never ran) -- see flushEventOutbox().
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
    void clearNotch (int channel, int index, ClearReason reason = ClearReason::Manual);
    void clearAll (ClearReason reason = ClearReason::ClearAll);

    // Owner decision D-05: a preset loaded mid-show is ADOPTED -- its notches
    // enter the model with Origin::Preset and auto-release treats them like
    // any other notch (30 s un-reinforced -> released). Each preset notch is
    // installed on ALL width_ lanes of this slot (design §2 sizes the command
    // burst as lanes x 16). Returns how many preset notches were adopted; a
    // notch whose parameters fail validation on a lane is skipped entirely.
    // `skippedOut` (optional) receives the count of notches naming a lane
    // this slot does not have -- today only lane 1 on a mono slot. A notch
    // that fails validation on every lane it targets is neither adopted nor
    // counted here.
    int adoptPreset (const std::vector<PresetNotch>& notches, int* skippedOut = nullptr);

    // Lane D (data loop): one event per notch set / clear, delivered to the
    // sink from the detector thread by flushEventOutbox(), or from the
    // caller of stop() after the join; never concurrently. Never from the
    // audio thread. Events queue in eventOutbox_ (cap kMaxPendingEvents, then
    // drop + count) and go out at the end of every runOnce() and once more
    // from stop(). With no sink the outbox is emptied, never grown.
    // setEventSink() requires the thread to be STOPPED, like setWidth().
    struct SpectralContext            // filled by Task 4 only
    {
        int    bins = Detector::kNumBins;
        double binHz = 0.0;
        double refAgeMs = 0.0;
        bool   hasRef = false, hasOther = false;
        std::array<float, Detector::kNumBins> now {}, ref {}, other {};
    };

    struct NotchEvent
    {
        // Retune: a depth change on a notch that stays where it is (lane G).
        // Neither a Set nor a Clear -- MainComponent::notchEventToVar MUST
        // give it its own `ev` name, or tools/logstats.py closes the notch's
        // record at the first 300 ms deepening (B-3).
        enum class Kind : std::uint8_t { Set, Clear, Retune };
        Kind  kind = Kind::Set;
        int   slot = 0, lane = 0, index = 0;
        float hz = 0.0f, q = 0.0f, depthDb = 0.0f;
        Origin      origin = Origin::Detector;              // Set, Retune
        ClearReason reason = ClearReason::Manual;           // Clear
        RetuneReason retuneReason = RetuneReason::Deepen;   // Retune
        float        fromDepthDb  = 0.0f;                   // Retune: the depth it left
        // Set: the RAW rise ratio the placement policy read (B-5). `rise`
        // below is rNorm, which SATURATES at rise 1.5 -- so it cannot tell a
        // 1.6 from a 40, and a test cannot use it to prove that a ramped
        // fixture actually landed in the -6 band rather than merely failing to
        // confirm. One float, filled from pc.breakdown.riseRatio, no
        // allocation, and not written for Clear or Retune.
        float        riseRatio    = 0.0f;
        double ageMs = 0.0;                                 // Clear/Retune: liveMs_ - lockedAtMs
        // Detector placements only (Task 4):
        bool  hasScore = false;
        int   confirmedLane = 0;
        float score = 0.0f, peakiness = 0.0f, pNorm = 0.0f, rise = 0.0f, novelty = 0.0f,
              penalty = 1.0f, asymmetry = 1.0f, thr = 0.0f;
        int   persistNeeded = 0;
        std::shared_ptr<const SpectralContext> ctx;   // null unless hasScore
    };
    using EventSink = std::function<void (const NotchEvent&)>;

    static constexpr int kMaxPendingEvents = 64;
    // Lifetime: the sink must outlive this controller's LAST stop() -- the
    // destructor calls stop(2000) and may invoke the sink from it. An owner
    // that captures `this` in the sink must call stop() on the controller
    // before its own members are destroyed, or call setEventSink(nullptr)
    // after that join.
    void setEventSink (EventSink sink);              // detector thread STOPPED; nullptr = off
    std::uint64_t droppedEvents() const;
    // TEST ACCESSORS ONLY
    int  pendingEventsForTest() const;
    bool modelMutexIsFreeForTest();                  // try_lock + unlock
    void failNextSetNotchOnLaneForTest (int lane);   // -1 = off

    // TEST ACCESSORS ONLY (lane G). The ladder lives entirely under
    // modelMutex_ and is otherwise observable only through emitted commands,
    // which cannot tell "did not move" from "moved and moved back".
    double depthDbForTest   (int channel, int index) const;
    double deepestDbForTest (int channel, int index) const;
    double quietMsForTest   (int channel, int index) const;
    // B-3: ModelNotch::active, NOT "depthDB < 0". pushClearLocked lowers only
    // `active` and leaves depthDB alone on purpose (the Clear event reads it),
    // so a depth-based liveness probe matches every slot that has ever held a
    // notch. Tasks 5-8 use this accessor for every "is a notch there?" check.
    bool   activeForTest    (int channel, int index) const;
    // Drives pushRetuneLocked the way the detector thread does, taking
    // modelMutex_ exactly once. Tasks 5-7 call the locked helper from loops
    // that already hold it; this seam is what lets the command path be tested
    // before those callers exist.
    bool   retuneForTest (int channel, int index, double newDepthDb, RetuneReason reason);
    // Fix-round 1 (review finding "Important 1"): ceilingDbFor and the rest of
    // ModelNotch's ladder state had no accessor at all, so nothing asserted
    // "Detector follows the live slider, everything else keeps its own depth"
    // (Q8), nor that a reused slot's stageChangedAtMs/releasedSteps/ceilingDb
    // are actually re-initialised alongside deepestDb/quietMs.
    //
    // ceilingDbForTest: the RESOLVED ceiling -- what ceilingDbFor(n) returns
    // (the live slider for a Detector notch, n.ceilingDb for everything else).
    double ceilingDbForTest       (int channel, int index) const;
    // rawCeilingDbForTest: the STORED field itself -- NaN for a Detector
    // notch, the caller's own depth otherwise. Distinct from ceilingDbForTest
    // so a test can tell "resolves to the slider" from "IS the slider".
    double rawCeilingDbForTest    (int channel, int index) const;
    int    releasedStepsForTest   (int channel, int index) const;
    double stageChangedAtMsForTest (int channel, int index) const;
    // Forces the pair the release freeze reads (spec 4.5 seam, M-6):
    // {valid, score}. nullopt restores the real frameScoreValid_/frameMaxScore_.
    // A static tone cannot hold score >= 0.385 for 30 s -- mNorm is a
    // log-ratio against a 3 s EMA and decays to 0 within seconds -- so the
    // freeze is untestable through audio alone.
    void   setRingRiskOverrideForTest (std::optional<std::pair<bool, float>> override);

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
        float depthDB   = 0.0f;   // the depth RUNNING right now
        // Lane G (Q11): the deepest rung this notch has ever stood on.
        // savePreset writes THIS rather than depthDB, so a preset saved while
        // the room is quiet still records what the room NEEDED, not what the
        // release ladder had wound back to.
        float deepestDb = 0.0f;
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

        // --- RING RISK readout (docs/spec-ring-risk.md §1) ---------------
        // The highest candidate confidence seen in the frame this snapshot
        // describes, 0 when no bin was scoreable. It is the SAME `score` the
        // placement decision compared against kConfirmScore -- post
        // asymmetry multiplier, max over every candidate of every analysed
        // lane of this slot (A-R1, A-R8) -- and never a second, separately
        // computed measure of the same thing.
        float ringRiskScore = 0.0f;
        // False until the detector actually scored this frame: detection
        // disarmed, or no lane had committed history to score against. The
        // GUI renders Unavailable while this is false rather than rendering
        // 0.0 as "low" -- an unwired indicator reading "low" is worse than
        // one reading "n/a", because a soundman would act on it.
        bool  ringRiskValid = false;
        // The live band line the GUI compares ringRiskScore against, so no
        // threshold is hardcoded on the GUI side (A-R3). Score is a 0..1
        // product, so this is CandidateScorer::kConfirmScore.
        float ringRiskThreshold = 0.0f;

        // Lane G (Q9): was the release clock frozen on the most recent tick?
        // Published for the GUI and the log to use LATER -- 1.2.0 draws
        // nothing with it. The RING RISK chip cannot stand in for it: it holds
        // for 750 ms and follows displayedSlot_ only, so it can read RISING
        // while the clock is running again, and a slot that is not displayed
        // can be frozen with nothing on screen saying so (M-8).
        bool  releaseFrozen = false;
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

        // --- lane G ladder state (spec 4.2) -------------------------------
        // All five are re-initialised by setNotchImpl, the ONE place a slot
        // becomes active, for every Origin and every path (placeConfirmed,
        // adoptPreset, the GUI's setNotch, the partial-apply unwind).
        // pushClearLocked only lowers `active`, so a reused slot would
        // otherwise inherit the previous tenant's ladder (B-2).

        // Deepest rung held since placement -- also where a reclamp jumps to.
        double deepestDb        = 0.0;
        // liveMs_ at the last depth change (deepen, release, reclamp, ceiling).
        double stageChangedAtMs = 0.0;
        // ACCUMULATED quiet time since the last depth change or reinforce, in
        // live ms. A counter rather than a timestamp precisely so the freeze
        // can stop it without losing what it had banked.
        double quietMs          = 0.0;
        // Rungs released from deepestDb. 0 == not releasing. An integer count
        // instead of comparing doubles (m-3).
        int    releasedSteps    = 0;
        // This notch's own ceiling. Preset/Manual/Soundcheck: the depth the
        // caller asked for -- the slider must not drag a notch a human or a
        // file set explicitly (Q8). Detector: NaN, meaning "follow the live
        // slider", resolved by ceilingDbFor().
        double ceilingDb        = 0.0;
    };

    static constexpr int slotOf (int channel, int index) { return channel * kSlots + index; }

    void flushOutbox();
    bool setNotchImpl (int channel, int index, double frequency, double Q, double depthDB,
                       Origin origin, const NotchEvent* scored);
    void pushClearLocked (int channel, int index, ClearReason reason);
    // modelMutex_ HELD. Re-sends `index` as a Set at a new depth, keeping the
    // notch's stored frequency, Q, lockedAtMs, origin and lastDetectedMs.
    // Applies the same five predicates setNotchImpl does plus the -24 floor,
    // and returns false changing NOTHING when any fails or the slot is not
    // active.
    //
    // It exists because both callers -- the reinforce loop in
    // processSpectrumForDetection and step 3 of runOnce -- already hold
    // modelMutex_, which is NOT recursive: calling setNotch/setNotchImpl from
    // either would deadlock, and setNotchImpl would also stamp a fresh
    // lockedAtMs, destroying lane D's age label (B-1).
    bool pushRetuneLocked (int channel, int index, double newDepthDb, RetuneReason reason);
    // The ceiling this notch obeys: its own, or the LIVE slider for a Detector
    // notch (whose ceilingDb is NaN). Read fresh every tick, so lowering the
    // slider mid-show takes effect (spec 7).
    double ceilingDbFor (const ModelNotch& n) const;
    void pushEventLocked (NotchEvent&& event);   // modelMutex_ HELD
    // modelMutex_ NOT held when the sink runs. Returns true if it delivered
    // (or attempted to deliver, with no sink) anything -- false when the
    // outbox was already empty. stop() loops on this so a sink re-entering
    // clearNotch()/setNotch() during the flush still gets drained.
    bool flushEventOutbox();

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

    // Lane D (data loop): everything placeConfirmed needs to build the scored
    // NotchEvent and its SpectralContext, gathered by the candidate loop
    // while the frame it scored is still current (see the .cpp comment on
    // refFrame's lifetime).
    struct PlacementContext
    {
        const float* now = nullptr;
        const float* other = nullptr;        // may be null
        CandidateScorer::ScoreBreakdown breakdown;
        float finalScore = 0.0f;             // after the asymmetry multiplier
        float asymmetry = 1.0f;
        int   persistNeeded = 0;
        float thr = 0.0f;
        double sampleRate = 0.0;
    };
    void placeConfirmed (int lane, const PeakinessAnalyzer::Candidate& cand, bool linkedNow,
                         const PlacementContext& pc);

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
        // Lane R (RING RISK validity, ruling A-R4): blocks this lane's scorer
        // has committed SINCE THE LAST RESET. It lives here, not in
        // CandidateScorer, precisely because the scorer must NOT be reset --
        // lane D removed that, and resetting it would change where notches
        // land. This counter is the readout's own memory: cleared by
        // setSampleRate() and setWidth(), so after a device/SR change the
        // chip reads N/A instead of publishing a number computed from rise
        // history and baseline EMAs that belong to the old rate, at bin
        // indices that now map to different frequencies. Saturating rather
        // than wrapping: a wrap to 0 would blink the chip to N/A once every
        // ~1.4 years of continuous running for no reason.
        std::uint32_t blocksSinceReset = 0;
    };
    std::array<LockFreeRingBuffer<float>*, kChannels> taps_ {};   // [0] never null
    std::array<LaneAnalysis, kChannels> lanes_;

    // Detector-thread-only state -- runOnce() is never entered concurrently,
    // so neither of these needs a lock or an atomic.
    //
    // drainIteration_ ticks once per lockstep drain iteration in runOnce().
    // linkedPlacedAt_[bin] carries the iteration in which a LINKED pair was
    // last placed over that bin, so the candidate loop can tell "the other
    // lane already placed this pair, THIS iteration" from "a fresh streak".
    // 0 is the never-placed sentinel: the counter starts at 1 and skips 0 on
    // wrap, so a stale zero can never match (see placeConfirmed's note).
    std::uint32_t drainIteration_ = 0;
    std::array<std::uint32_t, Detector::kNumBins> linkedPlacedAt_ {};

    // RING RISK accumulator for the drain iteration in flight (A-R2). The
    // per-lane detection pass writes them; runOnce() clears them before that
    // pass and publishes them with the same frame's magnitudes, so the score
    // and the spectrum the operator sees describe one instant.
    float frameMaxScore_   = 0.0f;
    bool  frameScoreValid_ = false;

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

    // Lane D. eventOutbox_ under modelMutex_; eventScratch_ is detector-thread
    // only (flushEventOutbox swaps them so the reserve() survives). eventSink_
    // is read under modelMutex_ by producers (pushEventLocked) and again,
    // unlocked, by flushEventOutbox() on the detector thread; it is written
    // ONLY by setEventSink() with the thread stopped, so both reads are a
    // benign same-value race and need no second mutex.
    EventSink eventSink_;
    std::vector<NotchEvent> eventOutbox_;
    std::vector<NotchEvent> eventScratch_;
    std::atomic<std::uint64_t> droppedEvents_ { 0 };
    std::atomic<int> failSetNotchLaneForTest_ { -1 };
    // Lane G seam (M-6): forces {frameScoreValid_, frameMaxScore_} for the
    // release freeze. Detector-thread state; written only with the thread
    // stopped, like setWidth()/setEventSink().
    std::optional<std::pair<bool, float>> ringRiskOverrideForTest_;

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
