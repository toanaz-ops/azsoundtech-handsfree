### Task 6: deepen and reclamp inside the reinforce loop

**Mức level dự kiến (spec §3):** a notch whose bin is STILL over the peakiness threshold gains one rung — **6 dB deeper** — every 300 ms of live time, ramped over 10 ms, until it reaches the ceiling's rung or the bin goes quiet. So a fast howl walks −12 → −18 in ~300 ms and −18 → −24 (ceiling permitting) in ~600 ms; the level at the bin ends up **the same as or deeper than 1.1.3**, just later. A notch that has begun releasing and is hit by the howl again jumps straight back to `deepestDb` in **one step of up to 18 dB** — deeper, immediately, with no 300 ms wait, because a returning howl is the emergency this whole feature exists for. Outside the bin: **0 dB**. Note M-9: deepening and "is it still howling" are the same test on the spectrum AFTER the notch, so the ladder STOPS at the first rung that quiets the bin (Q7) — a notch may legitimately live its whole life at −6 or −12 and never reach the ceiling. That is the "better tone" this lane was asked for, and it is also the row testers will notice.

**M-3 — the stopping behaviour is NOT testable in this suite, and no test here pretends it is.** The harness writes the raw tone straight into `h.tap` (`tests/test_notchcontroller.cpp:344-349`); nothing applies the notch chain between the tone and the Detector, so the analyser sees the UNNOTCHED spectrum on every frame. In these tests the bin therefore never goes quiet and the ladder always climbs to the ceiling — which is exactly what `AContinuingHowlDeepensOneRungPer300ms` asserts. The Q7 claim "stops at the first rung that quiets the bin" can only be observed on a real rig, so it belongs in the tester notes (Task 10), not in a fixture. **Do not build a fake notched-spectrum source to "cover" it**: a source that subtracts a modelled notch would be testing the model, not the loop, and would go green while the real chain did something else.

**Files:**
- Modify: `src/app/NotchController.cpp:737-763` (the reinforce loop inside `processSpectrumForDetection`)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Consumes (Task 4): `pushRetuneLocked`, `ceilingDbFor`, `nextDeeperRungDb` (two-arg, Q13), `activeForTest`, `kDeepenAfterMs`, `RetuneReason`, and the `ModelNotch` fields `deepestDb` / `stageChangedAtMs` / `quietMs` / `releasedSteps`. **Not** `retuneForTest` — rev 2 used it to fake a released notch and could not (M-A, below).
- Produces: nothing new in the public API. Task 7 relies on `quietMs` being ZEROED here on every reinforce, and on `releasedSteps` returning to 0 on a reclamp — and carries the test for that, because only Task 7 can put `releasedSteps` above 0 in the first place.

**Thread facts this task must respect:** the loop at `.cpp:737-763` already holds `modelMutex_` (taken at line 738), and `modelMutex_` is NOT recursive — `setNotch`/`setNotchImpl` take it themselves at `.cpp:162`, so calling either from here deadlocks (B-1). Everything goes through `pushRetuneLocked`. `liveMs_` only advances in step 2 of `runOnce`, AFTER the whole drain loop, so the 300 ms gate cannot fire twice within one drain. Under LINKED the loop visits both lanes from one frame (`.cpp:740-742`), which is what keeps a linked pair on the same rung.

**M-A — the reclamp branch is written HERE, but its behavioural test lives in Task 7,
and that is deliberate.** The rev-2 plan tested it here with
`retuneForTest(0, slot, -12.0, Release)` followed by one tone frame. That test cannot
pass: `retuneForTest` only forwards to `pushRetuneLocked`, which writes `n.depthDB`
and nothing else. `releasedSteps` is incremented by exactly one place — Task 7's
release branch — and `stageChangedAtMs` is not restamped either. So at the reinforce
pump `releasedSteps == 0`, the reclamp branch is skipped, the deepen branch runs
(`nextDeeperRungDb(-12, -24) = -18`) and the assertion reads **−18, not −24**.

Two ways out: widen `retuneForTest` to take a `releasedSteps`, or build the state
through the real path. **The plan takes the real path**, because a seam that can
fabricate `releasedSteps` is a seam that can hide the bug where the release ladder
fails to set it. The real path needs the release ladder, which does not exist until
Task 7 — so `AReturningHowlReclampsImmediatelyToDeepestDb` is **moved to Task 7**,
where it climbs to −24, sits quiet through 30 s + 10 s so the ladder walks
−24 → −18 → −12 with `releasedSteps == 2`, and only then takes one tone block.

The consequence to accept with open eyes: **the reclamp branch added in this task has
no red test in this task.** Task 6's `ctest` run therefore proves the deepen half only.
Do not "cover" it here with a seam that writes `releasedSteps` directly, and do not
reorder Tasks 6 and 7 — the release ladder reads `quietMs`, which this task is what
zeroes.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_notchcontroller.cpp`.

```cpp
// RED IF the ladder stops climbing while the bin is still over threshold
// (spec 4.4). The tone never stops, so every 300 ms of live time buys one
// rung until the ceiling's rung is reached.
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
// lane, each of which is a legitimate Set on a DIFFERENT index. That is
// documented on the stereo auto-release tests at :1101-1102 and is exactly the
// trap this assertion fell into. Assert on the depth of THIS slot, and on
// commands carrying this (lane, index) only.
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

// RED IF a LINKED pair can end up on different rungs. Both lanes are
// reinforced from the same frame (.cpp:740-742), so they must climb together.
TEST (NotchControllerLadder, LinkedLanesStayOnTheSameRung)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setDetectionActive (true);
    h.controller.setNotchDefaults (30.0, -24.0);

    NoiseSource quietL, quietR; quietR.rng.seed (999u);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());

    SineSource toneL, toneR;
    for (int i = 0; i < 200; ++i)
        pumpStereo (h, toneL.hop(), toneR.hop());

    // B-3: activeForTest, not `depthDbForTest < 0` -- a cleared slot keeps its
    // depth, so a depth probe would compare two dead lanes and pass on nothing.
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

// RED IF an INDEP placement touches the other lane (spec 5.3, "INDEP: làn kia
// không đổi") -- the half the LINKED test above cannot see. The right lane is
// quiet throughout, so nothing may appear on it at any rung.
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
        pumpStereo (h, toneL.hop(), quietR.hop());

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
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchControllerLadder --output-on-failure`
Expected: FAIL — `AContinuingHowlDeepensOneRungPer300ms` never leaves −12, `TheLastStepLandsExactlyOnAnOffRungCeiling` stays at −12, and `DeepeningStopsAtTheCeilingRung` never reaches −18. (Nothing in this run exercises the reclamp branch — see the M-A note above.)

- [ ] **Step 3: Implement the deepen/reclamp branch in `src/app/NotchController.cpp`**

**m-B / m-C — find this by ANCHOR, not by line number.** Task 5 inserts into
`placeConfirmed`, which sits ABOVE the reinforce loop, so every absolute number in
this task is pre-Task-5 and will already be wrong when you get here. Search for the
comment line

```
                // Same live threshold analyse() used to accept candidates --
```

(pre-Task-5 it is at `:752`) and replace from there down to and including the closing
`}` of the `if (PeakinessAnalyzer::peakinessAt(...) > la.analyzer.getThreshold())`
block (pre-Task-5 `:760`). **The stated region is `752-760`, not `755-760`:** the
replacement below re-emits those three comment lines, so replacing only the `if`
would leave the comment standing twice.

```cpp
                // Same live threshold analyse() used to accept candidates --
                // a notch re-tested against a stale default would disagree
                // with the panel about whether it is still reinforced.
                if (PeakinessAnalyzer::peakinessAt (block.magnitudes,
                                                    Detector::kNumBins, bin)
                        > la.analyzer.getThreshold())
                {
                    n.lastDetectedMs = liveMs_;
                    // Lane G: the bin is loud again, so the release clock's
                    // banked quiet time is spent, not merely paused.
                    n.quietMs        = 0.0;

                    if (n.releasedSteps > 0)
                    {
                        // RECLAMP (spec 4.4, Q3). No 300 ms gate: the ladder
                        // had already PROVEN this bin needs deepestDb, and a
                        // howl coming back is the emergency the whole feature
                        // exists for. One step, ramped over kRampMs like every
                        // other depth change -- it may be more than 6 dB, and
                        // that is deliberate (invariant 3 bounds the DEEP
                        // direction per step; this is a return to a depth this
                        // notch already ran at).
                        //
                        // M-B: the target is CLAMPED to the live ceiling. The
                        // ceiling is read fresh every tick, and the operator
                        // may have pulled the slider up while this notch was
                        // releasing -- Task 7's ceiling branch cannot have
                        // caught that, because a notch sitting SHALLOWER than
                        // the new ceiling never enters it. Without the max()
                        // a notch that climbed to -24, released to -6 under a
                        // slider then dropped to -12 would reclamp to -24:
                        // 12 dB past the ceiling, violating Q1 and spec 4.10
                        // invariant 2. max() picks the SHALLOWER value.
                        // For Preset/Manual, ceilingDbFor(n) IS their own
                        // depth, which equals deepestDb, so this is identity.
                        const double target = std::max (n.deepestDb, ceilingDbFor (n));
                        if (pushRetuneLocked (c, i, target, RetuneReason::Reclamp))
                        {
                            // Keep the memory and the depth in step: when the
                            // ceiling did not bind, target == deepestDb and
                            // this is a no-op; when it did, the notch must not
                            // go on remembering a rung it is no longer allowed
                            // to stand on. Task 7's per-tick clamp says the
                            // same thing from the other side.
                            n.deepestDb        = target;
                            n.releasedSteps    = 0;
                            n.stageChangedAtMs = liveMs_;
                        }
                    }
                    else if (n.origin == Origin::Detector)
                    {
                        // DEEPEN (spec 4.4). Only Detector notches climb:
                        // Preset/Manual said what they wanted (Q8) and
                        // Soundcheck is already excluded above (KD-7).
                        //
                        // M-9, and it is a CONSEQUENCE of Q2, not a bug: the
                        // test that decides "deepen" is the same test that
                        // decides "still howling", run on the spectrum AFTER
                        // the notch. So the ladder stops at the first rung
                        // that quiets the bin and may never reach the
                        // ceiling. That is the point -- depth by need.
                        //
                        // M-3: no headless test can SEE that, because the
                        // fixture feeds the analyser the un-notched tone. It
                        // is a rig claim; the tester notes carry it.
                        //
                        // Q13: the ceiling IS the last rung, and it need not
                        // be a multiple of 6 -- presets/Music.json ships -10,
                        // so this loop's final step there is 4 dB, not 6.
                        const double ceiling = ceilingDbFor (n);
                        if (n.depthDB > ceiling
                            && (liveMs_ - n.stageChangedAtMs) >= kDeepenAfterMs)
                        {
                            // nextDeeperRungDb caps at the ceiling itself, so
                            // the step can never overshoot it.
                            const double next = nextDeeperRungDb (n.depthDB, ceiling);
                            if (pushRetuneLocked (c, i, next, RetuneReason::Deepen))
                            {
                                n.deepestDb        = next;
                                n.stageChangedAtMs = liveMs_;
                            }
                        }
                    }
                }
```

- [ ] **Step 4: Run the controller tests**

Run: `cmake --build build --config Release && cd build && ctest -C Release -R NotchController --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Run the full suite**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` (497 — one fewer than rev 2: the reclamp test moved to Task 7, M-A).

- [ ] **Step 6: Commit**

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add src/app/NotchController.cpp tests/test_notchcontroller.cpp
```
```bash
git commit -m "feat(controller): deepen one rung per 300ms while the bin still rings; reclamp on return"
```

---

