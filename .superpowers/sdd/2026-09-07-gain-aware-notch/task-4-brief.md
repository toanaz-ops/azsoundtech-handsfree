### Task 4: the depth ladder in `NotchController` — model fields, clamp, `pushRetuneLocked`

**Mức level dự kiến (spec §3):** **0 dB** for detector placements — nothing calls the retune path yet (Tasks 5–7 do). The one behaviour change that ships here is Q12's clamp: a preset asking for a depth deeper than −24 dB (say −40) used to be adopted verbatim and is now cut to **−24 dB**, i.e. such a notch becomes **up to 16 dB shallower** than before. No shipped preset does this (`presets/Speech.json` is −18, `presets/Music.json` is −10), so only a hand-edited file is affected — and it now gets a log line saying so.

**Files:**
- Modify: `src/app/NotchController.h:74-95` (constants), `:180-196` (`NotchEvent`), `:207-215` (test accessors), `:268-306` (`SnapshotNotch`, `SnapshotBuffer`), `:320-336` (`ModelNotch`, private helpers), `:467` (override member)
- Modify: `src/app/NotchController.cpp:1-5` (includes), `:117` (ladder helpers), `:135-181` (`setNotchImpl`), `:212` (`pushRetuneLocked`), `:231-262` (`adoptPreset`), `:296-303` (snapshot notch gather), `:423` (test accessors)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Consumes: nothing from Tasks 1–3 (this task is model-side only).
- Produces:
  ```cpp
  // NotchController.h, public
  static constexpr double kDepthLadderDb[4] = { -6.0, -12.0, -18.0, -24.0 };
  static constexpr int    kDepthLadderSize  = 4;
  static constexpr double kDepthStepDb      = 6.0;
  static constexpr double kMaxDepthDb       = -24.0;
  static constexpr double kDeepenAfterMs    = 300.0;
  static constexpr float  kSteepRiseRatio   = 2.0f;
  static constexpr double kReleaseFirstMs   = kAutoReleaseMs;   // 30 000
  static constexpr double kReleaseStepMs    = 10000.0;
  static constexpr double kMemoryTtlMs      = 300000.0;
  static constexpr int    kMemoryEntriesPerLane = 16;
  static constexpr float  kRiskFreezeFraction   = 0.55f;

  enum class RetuneReason : std::uint8_t { Deepen, Release, Reclamp, Ceiling };

  // NotchEvent gains Kind::Retune plus:
  RetuneReason retuneReason = RetuneReason::Deepen;
  float        fromDepthDb  = 0.0f;
  float        riseRatio    = 0.0f;   // Set: the RAW rise the placement used (B-5)

  // Q13: the effective ladder is "rungs shallower than the ceiling, then the
  // ceiling". nextDeeperRungDb IS that ladder -- there is no separate
  // quantisation helper, and no `ceilingRungDb`.
  static double nextDeeperRungDb    (double currentDb, double ceilingDb);
  static double nextShallowerRungDb (double currentDb);

  double depthDbForTest   (int channel, int index) const;
  double deepestDbForTest (int channel, int index) const;
  double quietMsForTest   (int channel, int index) const;
  bool   activeForTest    (int channel, int index) const;   // B-3
  bool   retuneForTest    (int channel, int index, double newDepthDb, RetuneReason reason);
  void   setRingRiskOverrideForTest (std::optional<std::pair<bool, float>> override);

  struct SnapshotNotch { float frequency, Q, depthDB, deepestDb; std::uint8_t channel, index; };
  // SnapshotBuffer gains: bool releaseFrozen = false;

  // private, modelMutex_ HELD by the caller
  bool   pushRetuneLocked (int channel, int index, double newDepthDb, RetuneReason reason);
  double ceilingDbFor (const ModelNotch& n) const;   // == the ceilingRung (Q13)
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp`, after the last `NotchControllerRingRisk` test (the file ends at line 1681). `Harness`, `FakeClock`, `Recorder`, the `Ev` alias, `pump`, `SineSource`, `NoiseSource` and `primeAndPlace` already exist in that file — use those names.

```cpp
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
TEST (NotchControllerLadder, ReusingASlotResetsEveryLadderField)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -24.0,
                                        NotchController::Origin::Detector));
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -24.0);

    h.controller.clearNotch (0, 0, NotchController::ClearReason::Manual);
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -6.0,
                                        NotchController::Origin::Manual));

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, 0),   -6.0);
    EXPECT_DOUBLE_EQ (h.controller.deepestDbForTest (0, 0), -6.0);
    EXPECT_DOUBLE_EQ (h.controller.quietMsForTest (0, 0),    0.0);
}

// RED IF `activeForTest` is implemented as "depthDB < 0" (B-3). It must read
// ModelNotch::active, because pushClearLocked (NotchController.cpp:195-212)
// lowers ONLY `active` -- the Clear event reads n.depthDB at :208, so the
// depth is deliberately retained on a cleared slot. Every liveness probe in
// Tasks 5-8 is built on this accessor; a depth-based one would report every
// slot that has ever held a notch as still active, and the room-memory and
// ceiling tests would pass while asserting nothing.
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
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release 2>&1 | tail -8`
Expected: compile errors — `nextDeeperRungDb`, `nextShallowerRungDb`, `deepestDbForTest`, `activeForTest`, `retuneForTest`, `RetuneReason`, `Kind::Retune`, `fromDepthDb`, `riseRatio`, `SnapshotNotch::deepestDb` and `SnapshotBuffer::releaseFrozen` all undeclared.

- [ ] **Step 3: Extend `src/app/NotchController.h`**

Add `#include <optional>` and `#include <utility>` next to the existing includes (lines 42-48).

After `static constexpr double kDefaultNotchDepthDb = -18.0;` (line 95), add the lane-G constant block:

```cpp
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
    // gui::SpectrumView::kRingRiskRisingFraction (SpectrumView.h:239) is
    // changed in Task 7 to alias this name, so there is no second 0.55f
    // literal anywhere. Frozen at
    // score >= 0.55 x CandidateScorer::kConfirmScore = 0.385.
    //
    // Do not move this declaration into a .cpp or behind an accessor: the GUI
    // header includes app/NotchController.h (SpectrumView.h:45) and needs it
    // as a constant expression.
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
    //   `currentDb`, capped at `ceilingDb`. Saturates at the ceiling.
    // nextShallowerRungDb: the deepest fixed rung strictly shallower than
    //   `currentDb`, saturating at -6. Needs no ceiling -- a release always
    //   moves toward a fixed rung.
    static double nextDeeperRungDb (double currentDb, double ceilingDb);
    static double nextShallowerRungDb (double currentDb);
```

In `NotchEvent` (lines 180-188) replace the `Kind` enum and add the two fields:

```cpp
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
```

`placeConfirmed`'s `scored` event (`.cpp:620-627`) gains one line next to the other breakdown fields:

```cpp
    scored.novelty = pc.breakdown.mNorm; scored.penalty = pc.breakdown.penalty;
    scored.riseRatio = pc.breakdown.riseRatio;   // B-5: the RAW ratio, not rNorm
```

In the test-accessor block, after `void failNextSetNotchOnLaneForTest (int lane);` (line 210):

```cpp
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
    // Forces the pair the release freeze reads (spec 4.5 seam, M-6):
    // {valid, score}. nullopt restores the real frameScoreValid_/frameMaxScore_.
    // A static tone cannot hold score >= 0.385 for 30 s -- mNorm is a
    // log-ratio against a 3 s EMA and decays to 0 within seconds -- so the
    // freeze is untestable through audio alone.
    void   setRingRiskOverrideForTest (std::optional<std::pair<bool, float>> override);
```

In `SnapshotNotch` (lines 268-275):

```cpp
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
```

In `SnapshotBuffer`, after `float ringRiskThreshold = 0.0f;` (line 305):

```cpp
        // Lane G (Q9): was the release clock frozen on the most recent tick?
        // Published for the GUI and the log to use LATER -- 1.2.0 draws
        // nothing with it. The RING RISK chip cannot stand in for it: it holds
        // for 750 ms and follows displayedSlot_ only, so it can read RISING
        // while the clock is running again, and a slot that is not displayed
        // can be frozen with nothing on screen saying so (M-8).
        bool  releaseFrozen = false;
```

In `ModelNotch` (lines 320-329) add the five fields:

```cpp
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
```

Declare the private helpers next to `pushClearLocked` (line 336):

```cpp
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
```

And the override member, next to `failSetNotchLaneForTest_` (line 467):

```cpp
    std::optional<std::pair<bool, float>> ringRiskOverrideForTest_;
```

- [ ] **Step 4: Implement in `src/app/NotchController.cpp`**

Add `#include <optional>` and `#include <utility>` to the include block (lines 3-5).

Add the ladder helpers after `setWidth` (line 117):

```cpp
// --- Lane G ladder arithmetic (spec 4.1, Q13). Pure: no state, no locks. ---

double NotchController::nextDeeperRungDb (double currentDb, double ceilingDb)
{
    // The shallowest FIXED rung strictly deeper than `currentDb` ...
    double next = kDepthLadderDb[kDepthLadderSize - 1];
    for (int i = 0; i < kDepthLadderSize; ++i)
        if (kDepthLadderDb[i] < currentDb)
        {
            next = kDepthLadderDb[i];
            break;
        }
    // ... capped at the ceiling, which is the effective ladder's LAST rung
    // (Q13). std::max picks the SHALLOWER of the two, because deeper is more
    // negative: ceiling -10 turns "-6 -> -12" into "-6 -> -10".
    //
    // A caller must already have established `currentDb > ceilingDb` -- at or
    // past the ceiling this returns `currentDb` unchanged, which every caller
    // treats as "nothing to do" rather than as a step.
    return std::max (next, ceilingDb);
}

double NotchController::nextShallowerRungDb (double currentDb)
{
    // The deepest rung strictly shallower than `currentDb`, saturating at the
    // ladder top. An odd Preset/Manual depth resolves upward.
    for (int i = kDepthLadderSize - 1; i >= 0; --i)
        if (kDepthLadderDb[i] > currentDb)
            return kDepthLadderDb[i];
    return kDepthLadderDb[0];
}

// The `ceilingRung` of spec 4.1 -- under Q13 that IS the ceiling value, so
// there is nothing to quantise. A Detector notch (ceilingDb == NaN) follows
// the LIVE slider; everything else carries its own (Q8).
double NotchController::ceilingDbFor (const ModelNotch& n) const
{
    return std::isnan (n.ceilingDb) ? notchDepthDb_.load (std::memory_order_relaxed)
                                    : n.ceilingDb;
}
```

Rewrite `setNotchImpl` (lines 135-181) — the clamp goes in before validation, the five ladder fields inside the lock:

```cpp
bool NotchController::setNotchImpl (int channel, int index, double frequency, double Q, double depthDB,
                                    Origin origin, const NotchEvent* scored)
{
    // width gates the policy surface; internal fan-out loops never exceed it.
    if (channel < 0 || channel >= width_ || index < 0 || index >= kSlots)
        return false;

    // Q12 / invariant 1: nothing deeper than the ladder floor leaves this
    // controller, whatever asked for it -- a preset file is not a trusted
    // source, and -24 dB already kills any howl this app can hear. Applied
    // BEFORE validation, so a positive depth is still refused below.
    const double depth = std::max (depthDB, kMaxDepthDb);

    // Same predicates Biquad::setNotchFilter applies (see header comment).
    const double sampleRate = lanes_[0].detector.getSampleRate();
    if (! (sampleRate > 0.0))                       return false;
    if (! (Q > 0.0))                                return false;
    if (! (frequency > 0.0 && frequency < sampleRate * 0.5))
                                                    return false;
    if (! (depth <= 0.0))                           return false;  // positive depth would BOOST

    if (failSetNotchLaneForTest_.load (std::memory_order_relaxed) == channel)
    {
        failSetNotchLaneForTest_.store (-1, std::memory_order_relaxed);
        return false;   // TEST ONLY: forces the partial-apply unwind path
    }

    const NotchCommand cmd { NotchCommandType::Set,
                             (std::uint8_t) channel, (std::uint8_t) index,
                             (float) frequency, (float) Q, (float) depth,
                             slotId_ };

    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        auto& n = model_[slotOf (channel, index)];
        n.frequency      = frequency;
        n.Q              = Q;
        n.depthDB        = depth;
        n.origin         = origin;
        n.active         = true;
        n.lockedAtMs     = liveMs_;
        n.lastDetectedMs = liveMs_;
        // Lane G (B-2): the one place a slot becomes active is the one place
        // the ladder can be trusted to start clean.
        n.deepestDb        = depth;
        n.stageChangedAtMs = liveMs_;
        n.quietMs          = 0.0;
        n.releasedSteps    = 0;
        n.ceilingDb        = (origin == Origin::Detector)
                                 ? std::numeric_limits<double>::quiet_NaN()
                                 : depth;
        outbox_.push_back (cmd);

        NotchEvent ev = scored != nullptr ? *scored : NotchEvent {};
        ev.kind = NotchEvent::Kind::Set;
        ev.slot = slotId_; ev.lane = channel; ev.index = index;
        ev.hz = (float) frequency; ev.q = (float) Q; ev.depthDb = (float) depth;
        ev.origin = origin;
        pushEventLocked (std::move (ev));
    }
    return true;
}
```

Add `pushRetuneLocked` immediately after `pushClearLocked` (line 212):

```cpp
bool NotchController::pushRetuneLocked (int channel, int index, double newDepthDb,
                                        RetuneReason reason)
{
    auto& n = model_[slotOf (channel, index)];
    if (! n.active)
        return false;

    // Validate-before-send: the same five predicates as setNotchImpl plus the
    // ladder floor (spec 4.4). Nothing is touched when any of them fails -- a
    // refused retune leaves the model AND the chain on the depth they already
    // agreed on, which is the only state where they cannot disagree.
    const double sampleRate = lanes_[0].detector.getSampleRate();
    if (! (sampleRate > 0.0))                                    return false;
    if (! (n.Q > 0.0))                                           return false;
    if (! (n.frequency > 0.0 && n.frequency < sampleRate * 0.5)) return false;
    if (! (newDepthDb <= 0.0))                                   return false;
    if (! (newDepthDb >= kMaxDepthDb))                           return false;

    // freq and Q come from the STORED notch, never from notchQ_ (m-2): a Q
    // differing by one bit sends NotchChain::setNotch down the reset path, and
    // a reset mid-signal is the click this whole lane exists to avoid.
    outbox_.push_back ({ NotchCommandType::Set,
                         (std::uint8_t) channel, (std::uint8_t) index,
                         (float) n.frequency, (float) n.Q, (float) newDepthDb,
                         slotId_ });

    NotchEvent ev;
    ev.kind = NotchEvent::Kind::Retune;
    ev.slot = slotId_; ev.lane = channel; ev.index = index;
    ev.hz = (float) n.frequency; ev.q = (float) n.Q;
    ev.fromDepthDb = (float) n.depthDB;
    ev.depthDb     = (float) newDepthDb;
    ev.origin = n.origin;
    ev.retuneReason = reason;
    // Age from PLACEMENT, like a Clear's. lockedAtMs, origin and
    // lastDetectedMs are deliberately left alone: this is the same notch, and
    // lane D's labels are keyed to when it was placed (B-1).
    ev.ageMs = liveMs_ - n.lockedAtMs;
    pushEventLocked (std::move (ev));

    n.depthDB = newDepthDb;
    return true;
}
```

In `adoptPreset` (lines 231-262), count and log the clamps. Replace the opening line, and count inside the apply loop:

```cpp
    int adopted = 0, skipped = 0, clamped = 0;
```

```cpp
        int applied = 0;
        for (int lane = firstLane; lane <= lastLane; ++lane)
            if (setNotch (lane, p.index, p.freq, p.Q, p.depthDB, Origin::Preset))
            {
                ++applied;
                // Q12: setNotchImpl does the clamping; count it HERE so the
                // operator is told a hand-edited file asked for more than the
                // app allows, instead of quietly getting a different filter
                // than the file names.
                //
                // M-5: counted only when the notch was ACTUALLY adopted and
                // the clamp actually moved the value. Counting at the top of
                // the loop body -- above the `firstLane >= width_` skip at
                // .cpp:242-246 and above setNotchImpl's own validation --
                // reports clamps on notches that were never applied at all: a
                // lane-1 notch on a mono slot, a freq past Nyquist, a positive
                // depth. `lane == firstLane` makes it one count per
                // PresetNotch, not one per lane of a mirrored stereo pair.
                if (lane == firstLane && p.depthDB < kMaxDepthDb)
                    ++clamped;
            }
```

```cpp
    if (clamped > 0)
        juce::Logger::writeToLog ("preset: clamped " + juce::String (clamped)
                                  + " notch depth(s) to " + juce::String (kMaxDepthDb, 1)
                                  + " dB (slot " + juce::String (slotId_) + ")");
    if (skippedOut != nullptr)
        *skippedOut = skipped;
    return adopted;
```

In `runOnce`'s notch gather (lines 300-303) carry `deepestDb` into the snapshot:

```cpp
                    if (! n.active) continue;
                    notchList[notchCount++] = { (float) n.frequency, (float) n.Q,
                                                (float) n.depthDB, (float) n.deepestDb,
                                                (std::uint8_t) c, (std::uint8_t) i };
```

Add the accessors next to `liveMsForTest` (line 423):

```cpp
double NotchController::depthDbForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].depthDB;
}

double NotchController::deepestDbForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].deepestDb;
}

double NotchController::quietMsForTest (int channel, int index) const
{
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].quietMs;
}

bool NotchController::activeForTest (int channel, int index) const
{
    // B-3: the FLAG. pushClearLocked (.cpp:195-212) lowers `active` and leaves
    // frequency/Q/depthDB standing -- the Clear event reads n.depthDB at :208 --
    // so `depthDB < 0` is true for every slot that has ever held a notch.
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return model_[slotOf (std::clamp (channel, 0, kChannels - 1),
                          std::clamp (index, 0, kSlots - 1))].active;
}

bool NotchController::retuneForTest (int channel, int index, double newDepthDb,
                                     RetuneReason reason)
{
    if (channel < 0 || channel >= kChannels || index < 0 || index >= kSlots)
        return false;
    const std::lock_guard<std::mutex> lock (modelMutex_);
    return pushRetuneLocked (channel, index, newDepthDb, reason);
}

void NotchController::setRingRiskOverrideForTest (std::optional<std::pair<bool, float>> override)
{
    // Detector-thread state, written from the test thread with the detector
    // STOPPED -- same precondition as setWidth() and setEventSink().
    ringRiskOverrideForTest_ = override;
}
```

- [ ] **Step 5: Run the controller tests**

Run:
```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release -R NotchController --output-on-failure
```
Expected: PASS, including the eleven new `NotchControllerLadder.*` tests (the two ladder-arithmetic tests, `ActiveForTestReadsTheFlagNotTheRetainedDepth`, and the eight model/command tests).

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (483). No pre-existing test may move: nothing calls `pushRetuneLocked` outside the seam yet, and the only shipped behaviour change is the −24 clamp, which no shipped preset triggers.

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp
```
```bash
git commit -m "feat(controller): depth ladder state, -24 clamp for every origin, pushRetuneLocked"
```

---

