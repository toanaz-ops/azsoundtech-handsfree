### Task 7: the release ladder in `runOnce` step 3 — quiet clock, freeze, live ceiling

**Mức level dự kiến (spec §3):** this replaces 1.1.3's cliff (30 s un-reinforced → `Clear`, i.e. straight to 0 dB) with a staircase. From the last reinforce: **+6 dB at 30 s**, another **+6 dB at 40 s**, `Clear` at **50 s**. Between 30 s and 50 s the notch is therefore **DEEPER than 1.1.3** — 1.1.3 was already at 0 dB by then — so a tester hears up to **12 dB more tone missing** for 20 s after a howl stops. That is the direction of the trade Q3 chose, and it is one of the two rows testers will notice most. Beyond 50 s: identical to 1.1.3 (cleared, 0 dB). While RING RISK is at or above RISING (`score ≥ 0.55 × kConfirmScore = 0.385`) the clock **stops**, with **no time limit** (Q9), so in a room that stays tense the notch can hold its rung indefinitely — deeper than 1.1.3 for as long as the room is tense. A ceiling lowered mid-show pulls a Detector notch up to the new rung on the next tick, one ramped step, possibly more than 6 dB shallower at once (invariant 3 permits that). Outside the bin: **0 dB**.

**Files:**
- Modify: `src/app/NotchController.cpp:403-416` (step 3, replaced wholesale — the `releaseFrozen` publish is a NEW scope inside the replacement, not an edit to the snapshot block at `:361-372`. M-6: the v1 Files list cited `:369-371`, which is the `ringRiskScore`/`ringRiskValid`/`ringRiskThreshold` publish and is not touched by this task)
- Modify: `src/app/NotchController.h:83` (`kAutoReleaseMs` doc string)
- Modify: `src/gui/SpectrumView.h:239` (m-D: `kRingRiskRisingFraction` becomes an alias of `NotchController::kRiskFreezeFraction` instead of a second literal `0.55f` — Step 3)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Consumes (Task 4): `pushRetuneLocked`, `ceilingDbFor`, `nextShallowerRungDb`, `activeForTest`, `deepestDbForTest`, `kReleaseFirstMs`, `kReleaseStepMs`, `kDeepenAfterMs`, `kRiskFreezeFraction`, `ringRiskOverrideForTest_`, `SnapshotBuffer::releaseFrozen`, `quietMsForTest`, `liveMsForTest`. Consumes (Task 6): `quietMs` zeroed and `releasedSteps` reset on reinforce, and the reclamp branch itself — **M-A: this task carries the reclamp's only behavioural test** (`AReturningHowlReclampsImmediatelyToDeepestDb`), because `releasedSteps > 0` is a state only the release ladder in this task can produce.

**Release timing — the arithmetic every pump length in this task and Task 8 is derived from.** From the rung a notch STANDS on, `kReleaseFirstMs` (30 s) buys the first step and `kReleaseStepMs` (10 s) each one after, including the Clear:

| Standing at | Steps to −6 | Time to the first release | Time to Clear |
|---|---|---|---|
| −24 | 3 (−18, −12, −6) | 30 s | 30 + 10 + 10 + 10 = **60 s** |
| −18 | 2 (−12, −6) | 30 s | 30 + 10 + 10 = **50 s** |
| −13.7 | 2 (−12, −6) | 30 s | **50 s** |
| −12 | 1 (−6) | 30 s | 30 + 10 = **40 s** |
| −10 | 1 (−6) | 30 s | **40 s** |
| −6 | 0 | — | 30 s |

B-4: the v1 plan used `pumpQuietFor(..., 55000.0)` on notches standing at −24, which is 5 s short of the Clear. Every pump length below states the rung it was computed from.
- Produces: the `ClearReason::AutoRelease` moment Task 8 hangs room memory on.

**Thread facts this task must respect:** step 3 runs on the detector thread and takes `modelMutex_`. `snapshotMutex_` must NOT be taken inside it — publishing `releaseFrozen` therefore happens in its own scope BEFORE `modelMutex_` is acquired, keeping the two mutexes un-nested exactly as they are today (M-5). The freeze reads `frameScoreValid_` / `frameMaxScore_` (`NotchController.h:433-434`), which are detector-thread-only members holding precisely the two numbers the snapshot publish just wrote — no lock needed, and no new lock order created.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp`. Add this helper to the anonymous namespace next to `pump` (line 344) first:

```cpp
// Pumps ambient noise for `ms` of LIVE time. The tap stays alive (D-06) so
// the release clock runs, and the noise never feeds peakiness at 1 kHz.
void pumpQuietFor (Harness& h, NoiseSource& quiet, double ms)
{
    const int blocks = (int) std::lround (ms / kBlockMs);
    for (int i = 0; i < blocks; ++i)
        pump (h, quiet.hop());
}
```

```cpp
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
    // for the Clear event to read (NotchController.cpp:208).
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

// RED IF RAISING the ceiling deepens a notch on its own (spec 5.3: "không đào
// lại khi trần nâng về -18 cho tới khi reinforce"). The ceiling branch is a
// one-way valve -- it pulls a notch UP to a lowered ceiling, and a raised one
// buys nothing until the bin actually rings again and the deepen path (Task 6)
// runs. Detection is OFF here precisely so nothing can reinforce.
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
// exact case that ships: tests/test_gui_wiring.cpp:1030 adopts a preset notch
// at -9.0, which is not a ladder rung. Under spec v2's quantisation
// (ceilingRungDb(-9) == -6) the FIRST runOnce() after adoption would have seen
// depthDB -9 < rung -6 and retuned it to -6 -- a preset silently 3 dB
// shallower than the file says, and a violation of Q8 / spec 4.1 "slider
// không chạm". Q13 removes the quantisation, and the Origin guard means the
// branch cannot come back even if the arithmetic changes again.
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
// target drifts off its own depth (spec 5.3: "Preset: adoptPreset -12 => không
// đào; nhả thang; kẹp lại về -12"). Q8 makes the slider unable to touch it; it
// does NOT make it immortal -- only Soundcheck is (KD-7).
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
// because the state it needs can only be built by the RELEASE ladder.
// retuneForTest forwards to pushRetuneLocked, which writes depthDB and
// nothing else -- releasedSteps stays 0, the reclamp branch is skipped, the
// deepen branch runs instead and the notch reads -18. So the shallow rung is
// reached the way a room reaches it: by going quiet.
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

    NotchCommand drained {};
    while (h.commands.read (&drained, 1) == 1) {}
    const double before = h.controller.liveMsForTest();

    pump (h, tone.hop());              // ONE frame of howl at that bin

    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -24.0)
        << "the reclamp waited instead of firing on the frame";
    EXPECT_LT (h.controller.liveMsForTest() - before, NotchController::kDeepenAfterMs);
    EXPECT_DOUBLE_EQ (h.controller.quietMsForTest (0, slot), 0.0);

    // releasedSteps has no accessor, and does not need one: its whole meaning
    // is "which threshold does the next release use". After a reclamp the
    // notch must be back on the 30 s FIRST-rung threshold. If releasedSteps
    // had stayed at 2 this would release at 10 s and read -18 here.
    pumpQuietFor (h, quiet, 29000.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -24.0)
        << "the reclamp left releasedSteps standing, so the notch released "
           "again after 10 s instead of 30";
    pumpQuietFor (h, quiet, 1500.0);
    EXPECT_DOUBLE_EQ (h.controller.depthDbForTest (0, slot), -18.0);
}

// RED IF a lowered ceiling fails to cap what a later reclamp can do (M-B).
// This is the ONE sequence that reaches the unconditional deepestDb clamp:
// the notch ends up SHALLOWER than the new ceiling, so the Set(ceilingRung)
// branch never runs and cannot do the clamping for it. Without the fix the
// notch reclamps to -24 under a -12 ceiling -- 12 dB louder a cut than the
// operator asked for, on a live PA (Q1, spec 4.10 invariant 2).
//
// Built by hand rather than through primeAndPlace so the notch is on a known
// index while detection is OFF for the whole release; SineSource's bin 43 is
// 1007.8125 Hz, so the reinforce loop finds it once detection is armed.
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
    // -6 is NOT deeper than -12, so the Set(ceilingRung) branch is skipped --
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
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchControllerLadder --output-on-failure`
Expected: FAIL — `ReleaseWalksTheLadderAt30sThen10sPerRung` finds the notch already cleared at 30.5 s (today's cliff), the freeze tests all release regardless of the override, `AReturningHowlReclampsImmediatelyToDeepestDb` finds the notch cleared long before its 42 s pump ends, and `ALoweredCeilingAlsoCapsTheReclampTarget` the same.

- [ ] **Step 3: Publish `releaseFrozen`'s neighbours, update the constant's doc, and collapse the duplicate 0.55**

**m-D: `kRiskFreezeFraction`'s own doc string says "one constant, not two", and rev 2
still left two.** `gui::SpectrumView::kRingRiskRisingFraction` (`src/gui/SpectrumView.h:239`)
is a second `0.55f` literal that means exactly the same thing. `SpectrumView.h`
already includes `app/NotchController.h` (line 45 — verify with
`rg "NotchController.h" src/gui`, which reports that include and one in
`NotchListPanel.h:41`), so the GUI constant can simply BE the controller's:

```cpp
    // Where Rising starts, as a fraction of the threshold the DETECTOR
    // published (snapshot.ringRiskThreshold). Nothing here hardcodes that
    // threshold: it tracks the RESPONSE preset instead of being one more
    // magic constant, and a GUI-side constant would silently disagree with
    // the machine it describes the moment the DSP side moved.
    //
    // Lane G (m-D): it is now literally the same constant. NotchController's
    // release-clock freeze uses this same band line (spec 4.5, Q4), so the
    // value is DEFINED there and aliased here. Two 0.55f literals with one
    // meaning is exactly the drift the comment above warns about, and a test
    // asserting they are equal would only be checking that nobody edited one
    // of them -- this makes the question unaskable instead.
    static constexpr float kRingRiskRisingFraction = NotchController::kRiskFreezeFraction;
```

No test is added for the equality: the alias makes the two names the same object, so
a `static_assert` or an `EXPECT_FLOAT_EQ` would be a tautology. What *is* worth
checking is that the GUI still bands correctly, and `riskForScore`'s existing tests
already do that.

This is a header change, so it needs a full reconfigure + build (CLAUDE.md build
table), and `src/gui/SpectrumView.h` joins the commit.

In `src/app/NotchController.h:82-83`, replace the `kAutoReleaseMs` comment:

```cpp
    // Spec 5.2 step 7 as amended by lane G (spec 4.5, Q3): 30 s of quiet buys
    // the FIRST rung of the release ladder, not a Clear. kReleaseStepMs
    // (10 s) buys each rung after it, and only the notch already sitting at
    // -6 dB is cleared. The name and value are kept because this is still
    // "how long the first release takes"; kReleaseFirstMs is its lane-G alias.
    static constexpr double kAutoReleaseMs       = 30000.0;
```

- [ ] **Step 4: Replace step 3 in `src/app/NotchController.cpp`**

Replace lines 403-416 in `runOnce` with:

```cpp
    // 3. Release ladder (spec 4.5), replacing 1.1.3's "30 s -> Clear" cliff.

    // --- step 1: FREEZE ----------------------------------------------------
    // Computed and PUBLISHED unconditionally, above the tapAlive gate (M-6).
    // The v1 plan wrote latest_.releaseFrozen only while the tap was alive, so
    // the flag kept whatever it last said once the tap died -- a GUI or a log
    // reader would then be told "frozen" about a slot that has no audio at
    // all. A dead tap is not a frozen clock; it is no clock.
    //
    // Read the two detector-thread members the snapshot publish above just
    // wrote. snapshotMutex_ is deliberately NOT taken while modelMutex_ is
    // held: model -> snapshot would be a lock order this app has never had,
    // and inventing one for a readout is not a trade worth making (M-5).
    // These members are written and read only on this thread.
    bool  riskValid = frameScoreValid_;
    float riskScore = frameMaxScore_;
    if (ringRiskOverrideForTest_.has_value())
    {
        riskValid = ringRiskOverrideForTest_->first;
        riskScore = ringRiskOverrideForTest_->second;
    }
    // The GUI's RISING band, from ONE shared constant. An INVALID reading
    // never freezes (invariant 7): detection off must release exactly as
    // 1.1.3 did, or turning detection off would strand every notch.
    const bool frozen = tapAlive
                     && riskValid
                     && riskScore >= kRiskFreezeFraction * CandidateScorer::kConfirmScore;

    // Its own scope, before modelMutex_ is taken anywhere below, so the two
    // mutexes stay un-nested. Q9: no time cap on the freeze, and 1.2.0 draws
    // nothing with this flag -- it exists so a later GUI or the session log
    // can say WHY a notch is not winding back.
    {
        const std::lock_guard<std::mutex> lock (snapshotMutex_);
        latest_.releaseFrozen = frozen;
    }

    //    The ladder itself is only meaningful while audio actually flows (D-06).
    if (tapAlive)
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        for (int c = 0; c < kChannels; ++c)
            for (int i = 0; i < kSlots; ++i)
            {
                auto& n = model_[slotOf (c, i)];
                // KD-7: soundcheck notches never release, never deepen, never
                // follow the ceiling. They clear only on an explicit request.
                if (! n.active || n.origin == Origin::Soundcheck)
                    continue;

                // --- live ceiling (spec 4.1) -------------------------------
                // The ceiling is read fresh every tick, so an operator who
                // pulls the depth slider up mid-show sees it take effect.
                //
                // B-1: DETECTOR ONLY, and the guard is load-bearing, not
                // decoration. Q8 / spec 4.1 say the slider must not touch a
                // Preset or Manual notch, and the shipping path proves it
                // matters: tests/test_gui_wiring.cpp:1030 adopts a preset
                // notch at -9.0. Under spec v2's quantisation the ceiling for
                // that notch resolved to -6, so `-9 < -6` fired on the very
                // first runOnce() and pulled a preset 3 dB shallower than its
                // file. Q13 removes the quantisation (ceilingDbFor returns -9
                // and the comparison is false), but the Origin test is what
                // keeps this branch off Preset/Manual whatever the arithmetic
                // does next.
                //
                // Raising the ceiling deepens nothing here, deliberately: the
                // comparison is one-way, so a raised ceiling only buys depth
                // through Task 6's reinforce path (spec 5.3).
                const double ceiling = ceilingDbFor (n);
                if (n.origin == Origin::Detector)
                {
                    // M-B: clamp the MEMORY unconditionally, every tick, even
                    // when depthDB is already shallower than the ceiling and
                    // the Set below will not run. This line is the whole fix.
                    //
                    // The hole it closes: climb to -24 under a -24 slider
                    // (deepestDb -24) -> go quiet, release all the way to -6
                    // (deepestDb untouched, that is the point of it) ->
                    // operator drops the slider to -12. The test below reads
                    // `-6 < -12` = FALSE, so without this line nothing
                    // happens and deepestDb stays -24 -- and the next returning
                    // howl reclamps to -24, 12 dB past the ceiling. That
                    // violates Q1 and spec 4.10 invariant 2, and it is a
                    // LOUDER-than-asked-for outcome on a live PA, which is the
                    // one direction this codebase does not get to be sloppy in.
                    //
                    // max() picks the SHALLOWER of the two (deeper is more
                    // negative). It never deepens deepestDb: a RAISED ceiling
                    // leaves max(deepestDb, ceiling) == deepestDb.
                    n.deepestDb = std::max (n.deepestDb, ceiling);

                    if (n.depthDB < ceiling)
                    {
                        // One ramped step, possibly more than 6 dB, and
                        // possibly landing off-rung (Q13: the ceiling IS the
                        // last rung). Invariant 3 bounds the DEEP direction
                        // only -- shallower is never dangerous.
                        if (pushRetuneLocked (c, i, ceiling, RetuneReason::Ceiling))
                        {
                            n.stageChangedAtMs = liveMs_;
                            n.quietMs          = 0.0;   // a depth change restarts the clock
                        }
                        continue;   // one depth change per notch per tick
                    }
                }

                // --- steps 2-4: the quiet clock and the ladder -------------
                if (! frozen)
                    n.quietMs += dt;

                const double needed = (n.releasedSteps == 0) ? kReleaseFirstMs
                                                             : kReleaseStepMs;
                if (n.quietMs < needed)
                    continue;

                if (n.depthDB < kDepthLadderDb[0])
                {
                    // Up one rung. An odd Preset/Manual depth resolves to the
                    // nearest rung above it.
                    const double next = nextShallowerRungDb (n.depthDB);
                    if (pushRetuneLocked (c, i, next, RetuneReason::Release))
                    {
                        ++n.releasedSteps;
                        n.stageChangedAtMs = liveMs_;
                        n.quietMs          = 0.0;
                    }
                }
                else
                {
                    // Already on the shallowest rung: the notch has nothing
                    // left to give back, so it goes. Task 8 records what this
                    // bin needed before the Clear.
                    pushClearLocked (c, i, ClearReason::AutoRelease);
                }
            }
    }
```

- [ ] **Step 5: Run the controller tests**

Step 3 touched two headers (`NotchController.h`, `SpectrumView.h`), so this run
reconfigures first:

Run: `cmake -B build -G "Visual Studio 18 2026" -A x64 && cmake --build build --config Release && cd build && ctest -C Release -R NotchController --output-on-failure`
Expected: PASS. **Five** pre-existing tests now take LONGER to clear and must be checked, not "fixed" — the behaviour they assert (a Clear eventually arrives, carrying `AutoRelease`) is still correct, only the clock moved. Every new pump length is computed from the rung the notch actually stands on, using the table in this task's header.

**1. `NotchControllerAutoRelease.LiveTapReleasesAfter30s`** (line 152, notch at −12, pumps 7000 × 5 ms = 35 s). From −12 the Clear is at 40 s:

```cpp
TEST (NotchControllerAutoRelease, LiveTapReleasesThroughTheLadderAfter40s)
{
    Harness h;
    ASSERT_TRUE (h.controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Detector));
    std::vector<float> hop (512, 0.1f);
    // From -12: 30 s buys -6, +10 s buys the Clear = 40 s. 8800 * 5 ms = 44 s.
    for (int i = 0; i < 8800; ++i) {
        h.tap.write (hop.data(), hop.size());
        h.clock.advance (5.0);
        h.controller.runOnce();
    }
    h.controller.runOnce();                    // make sure everything is flushed

    NotchCommand cmd {};
    bool sawClearOfSlot0 = false;
    while (h.commands.read (&cmd, 1) == 1)
        if (cmd.type == NotchCommandType::Clear && cmd.channel == 0 && cmd.index == 0)
            sawClearOfSlot0 = true;
    EXPECT_TRUE (sawClearOfSlot0);
}
```

**2. `NotchControllerPreset.AdoptedPresetsAutoReleaseLikeDetectorNotches`** (line 267) — **the plan missed this one entirely.** It adopts a preset notch at −12 and pumps 7000 × 5 ms = 35 s expecting a Clear on both lanes. Under the ladder a −12 notch releases to −6 at 30 s and clears at 40 s, so 35 s finds it alive at −6 and the test goes red. Same fix, same arithmetic:

```cpp
    // Preset notches ride the lane-G release ladder like any other non-
    // Soundcheck notch (only KD-7 exempts anything). From -12: -6 at 30 s,
    // Clear at 40 s. 8800 * 5 ms = 44 s.
    for (int i = 0; i < 8800; ++i) {
```

**3. `NotchControllerDetection.HowlThatStopsAutoReleasesAfter30s`** (line 471, `3500` blocks ≈ 37.3 s). The notch there is placed by the detector, so after Task 5 + Task 6 it stands at the **ceiling** — `kDefaultNotchDepthDb` is −18 (`NotchController.h:95`), giving a Clear at 50 s. Take the block count to `5200` (≈ 55.5 s) and rename to `…AutoReleasesThroughTheLadder`.

**4/5. The two stereo auto-release tests** at lines 1106 (`IndepAutoReleaseIsPerLane`) and 1133 (`LinkedAutoReleaseWaitsForBothLanes`). m-6: both use `kAutoReleaseMs / kBlockMs + 20`, **not** `+ 10` — only `EveryClearPathCarriesItsReason` at line 1325 uses `+ 10`. Their notches are detector-placed against the −18 default ceiling, so:

```cpp
    // From the -18 ceiling: 30 + 10 + 10 = 50 s to Clear. The + 20 blocks of
    // slack the 1.1.3 version carried are kept.
    const int blocks = (int) ((NotchController::kReleaseFirstMs
                              + 2 * NotchController::kReleaseStepMs) / kBlockMs) + 20;
```

**6. `NotchControllerEvents.EveryClearPathCarriesItsReason`**'s AutoRelease case (line 1320). Its notch is a **Manual** −12, so its own depth is its ceiling and the Clear is at 40 s. Line 1325's `+ 10` becomes:

```cpp
        // Manual -12: -6 at 30 s, Clear at 40 s.
        const int blocks = (int) ((NotchController::kReleaseFirstMs
                                  + NotchController::kReleaseStepMs) / kBlockMs) + 10;
```

The test also asserts `EXPECT_GT (c[0].ageMs, NotchController::kAutoReleaseMs)` — still true (40 s > 30 s) and left alone. If any of these five now reports TWO clears, the ladder is emitting a Clear per rung instead of a Retune: fix the loop, not the test.

- [ ] **Step 6: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (512).

- [ ] **Step 7: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.h src/app/NotchController.cpp src/gui/SpectrumView.h tests/test_notchcontroller.cpp
```
```bash
git commit -m "feat(controller): release ladder with a freezable quiet clock and a live ceiling"
```

---

