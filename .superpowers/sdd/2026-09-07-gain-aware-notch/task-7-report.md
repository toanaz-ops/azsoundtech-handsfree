# Task 7 report — the release ladder in `runOnce` step 3

Commit `5638555` — `feat(app): release ladder — 30 s then 10 s per rung, frozen while RING RISK ≥ RISING, live ceiling`
Branch `claude_desk/lane-g-brainstorm-sdd-f3c568`, worktree
`D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-g-brainstorm-sdd-f3c568`.

**Expected level change (unchanged from the brief):** between 30 s and 50 s after the
last howl the notch still cuts −12 then −6 where 1.1.3 already cut 0 dB — up to 12 dB
more tone missing for ~20 s. No release at all while RING RISK is at or above RISING,
with no time cap (Q9). Beyond the bottom of the ladder: identical to 1.1.3 (Clear,
0 dB). Outside the bin: 0 dB. Soundcheck notches are exempt (KD-7).

---

## 1. What was implemented

### `src/app/NotchController.cpp` — step 3 of `runOnce`, replaced wholesale

The 1.1.3 cliff

```cpp
if (n.active && n.origin != Origin::Soundcheck
    && (liveMs_ - n.lastDetectedMs) > kAutoReleaseMs)
    pushClearLocked (c, i, ClearReason::AutoRelease);
```

became, in order:

1. **Freeze, computed above the `tapAlive` gate.** `riskValid`/`riskScore` read the
   detector-thread-only members `frameScoreValid_` / `frameMaxScore_`
   (`ringRiskOverrideForTest_` substitutes for both when set).
   `frozen = tapAlive && riskValid && riskScore >= kRiskFreezeFraction *
   CandidateScorer::kConfirmScore` (0.55 × 0.7 = 0.385).
2. **`latest_.releaseFrozen = frozen` published in its own `snapshotMutex_` scope,
   BEFORE `modelMutex_` is taken anywhere.** Written on *every* `runOnce`, so a dead
   tap publishes `false` rather than leaving the last value standing.
3. **`if (tapAlive)` → one `modelMutex_` scope** over all `kChannels × kSlots`. Per
   active non-Soundcheck notch:
   - **Ceiling pass, Detector only.** `n.deepestDb = std::max (n.deepestDb, ceiling)`
     runs **unconditionally, every tick** (M-B), *before* the Set below, so the reclamp
     branch in `processSpectrumForDetection` can never see a `deepestDb` the ceiling
     has already outlawed. Then `if (n.depthDB < ceiling)` →
     `pushRetuneLocked (…, ceiling, RetuneReason::Ceiling)`, stamp `stageChangedAtMs`,
     zero `quietMs`, `continue` (one depth change per notch per tick). Preset / Manual
     / Soundcheck never enter this branch — the `Origin::Detector` guard is what keeps
     the slider off a depth a human or a file set (Q8).
   - **Quiet clock.** `if (! frozen) n.quietMs += dt;`
   - **Threshold.** `needed = (releasedSteps == 0) ? kReleaseFirstMs : kReleaseStepMs`
     (30 s / 10 s).
   - **On reaching it.** `depthDB < kDepthLadderDb[0]` (−6) →
     `pushRetuneLocked (nextShallowerRungDb (depthDB), RetuneReason::Release)`,
     `++releasedSteps`, stamp `stageChangedAtMs`, `quietMs = 0`. Otherwise →
     `pushClearLocked (…, ClearReason::AutoRelease)`.

`n.lastDetectedMs` is no longer read by step 3; it is still written by the reinforce
loop and read by lane D's event ages, so it was left alone.

### `src/app/NotchController.cpp` — reinforce loop, edge (a) only

The one in-scope change outside step 3:

```cpp
const double target = std::max (n.deepestDb, ceilingDbFor (n));
if (target == n.depthDB
    || pushRetuneLocked (c, i, target, RetuneReason::Reclamp))
{   /* deepestDb = target; releasedSteps = 0; stageChangedAtMs = liveMs_; */ }
```

### `src/app/NotchController.h`
`kAutoReleaseMs`'s doc string now says it buys the **first rung**, not a Clear. Value
and name unchanged.

### `src/gui/SpectrumView.h`
`kRingRiskRisingFraction` is now `NotchController::kRiskFreezeFraction`, not a second
`0.55f` literal. No change was needed in `tests/test_spectrumview.cpp` — it exercises
`riskForScore`, not the constant — so that file is not in the commit.

---

## 2. TDD evidence

Tests written first, built, run RED before a line of implementation existed:

```
[==========] Running 110 tests from 14 test suites.
[  FAILED  ] NotchControllerLadder.ReleaseWalksTheLadderAt30sThen10sPerRung
[  FAILED  ] NotchControllerLadder.TheBottomOfTheLadderClearsWithAutoRelease
[  FAILED  ] NotchControllerLadder.RingRiskAtRisingFreezesTheReleaseClock
[  FAILED  ] NotchControllerLadder.InvalidRingRiskDoesNotFreezeTheClock
[  FAILED  ] NotchControllerLadder.ScoreJustBelowTheRisingBandDoesNotFreeze
[  FAILED  ] NotchControllerLadder.LoweringTheCeilingPullsADetectorNotchUpOnTheNextTick
[  FAILED  ] NotchControllerLadder.RaisingTheCeilingDoesNotDeepenUntilTheBinRingsAgain
[  FAILED  ] NotchControllerLadder.APresetNotchReleasesDownTheLadderAndReclampsToItsOwnDepth
[  FAILED  ] NotchControllerLadder.AReturningHowlReclampsImmediatelyToDeepestDb
[  FAILED  ] NotchControllerLadder.ALoweredCeilingAlsoCapsTheReclampTarget
[  FAILED  ] NotchControllerLadder.ACeilingLevelWithTheReleasedDepthReclampsWithoutResendingIt
[  FAILED  ] NotchControllerLadder.ACeilingRaisedAboveTheDeepestRungPullsUpAsCeilingNotReclamp
[  PASSED  ] 98 tests.
[  FAILED  ] 12 tests
```

`AnOffRungPresetDepthSurvivesTheFirstTick`, `SoundcheckNotchesNeverRelease` and
`NoCommandEverLeavesTheLegalDepthRange` were green from the start — they assert that
something does *not* happen, and 1.1.3's cliff did not do it either. They are kept as
regression pins on the new branch, not as red-first evidence.

The five pre-existing auto-release tests whose pump lengths moved were also green
before the implementation (a longer pump still clears under a cliff), which is the
correct shape: the claim they make is unchanged, only the clock moved.

### Green, focused

```
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter='NotchController*:SpectrumView*'
[==========] 131 tests from 15 test suites ran. (9265 ms total)
[  PASSED  ] 130 tests.        <- one failure, fixed below
```

after fixing `AReturningHowlReclampsImmediatelyToDeepestDb` (see §4), the same filter
and then the full gate:

```
$ cd build && ctest -C Release
100% tests passed, 0 tests failed out of 516
Total Test time (real) =  49.45 sec
```

500 → 516: 16 new `NotchControllerLadder` tests.

---

## 3. Tests added / changed

**New (16), all `NotchControllerLadder`:**

| Test | Pins |
|---|---|
| `ReleaseWalksTheLadderAt30sThen10sPerRung` | 30 / 10 / 10 timings and the Clear at the bottom |
| `TheBottomOfTheLadderClearsWithAutoRelease` | one Clear, reason `AutoRelease`, `depthDb −6`, exactly two `Release` retunes |
| `RingRiskAtRisingFreezesTheReleaseClock` | 90 s frozen leaves `quietMs == 0.0`; `snapshot.releaseFrozen` true; clock resumes |
| `InvalidRingRiskDoesNotFreezeTheClock` | invariant 7 — an invalid reading is not a freeze |
| `ScoreJustBelowTheRisingBandDoesNotFreeze` | the band line is `>=`, not `>` of something lower |
| `LoweringTheCeilingPullsADetectorNotchUpOnTheNextTick` | live ceiling, `Ceiling` retune −18 → −12 |
| `RaisingTheCeilingDoesNotDeepenUntilTheBinRingsAgain` | the ceiling branch is a one-way valve |
| `LoweringTheCeilingLeavesPresetAndManualNotchesAlone` | Q8 |
| `AnOffRungPresetDepthSurvivesTheFirstTick` | B-1 — the −9 preset `test_gui_wiring.cpp` ships |
| `APresetNotchReleasesDownTheLadderAndReclampsToItsOwnDepth` | presets release; reclamp to the file's depth |
| `AReturningHowlReclampsImmediatelyToDeepestDb` | M-A — reclamp inside the 300 ms gate, `releasedSteps` back to 0 |
| `ALoweredCeilingAlsoCapsTheReclampTarget` | M-B — the unconditional `deepestDb` clamp |
| `ACeilingLevelWithTheReleasedDepthReclampsWithoutResendingIt` | **edge (a)** |
| `ACeilingRaisedAboveTheDeepestRungPullsUpAsCeilingNotReclamp` | **edge (b)** |
| `SoundcheckNotchesNeverRelease` | KD-7 against the ladder specifically |
| `NoCommandEverLeavesTheLegalDepthRange` | invariant 1 over 200 randomised reinforce/quiet/frozen blocks |

Plus `pumpQuietFor (Harness&, NoiseSource&, double ms)` in the anonymous namespace
next to `pump`.

**Changed (6 pre-existing), pump lengths only — every one recomputed from the rung the
notch actually stands on:**

| Test | Was | Now |
|---|---|---|
| `NotchControllerAutoRelease.LiveTapReleasesAfter30s` | 7000 × 5 ms | renamed `…LiveTapReleasesThroughTheLadderAfter40s`, 8800 × 5 ms (−12 ⇒ Clear at 40 s) |
| `NotchControllerPreset.AdoptedPresetsAutoReleaseLikeDetectorNotches` | 7000 × 5 ms | 8800 × 5 ms |
| `NotchControllerDetection.HowlThatStopsAutoReleasesAfter30s` | 3500 blocks | renamed `…HowlThatStopsAutoReleasesThroughTheLadder`, 5200 blocks (−18 ceiling ⇒ 50 s) |
| `NotchControllerStereo.IndepAutoReleaseIsPerLane` | `kAutoReleaseMs / kBlockMs + 20` | `(kReleaseFirstMs + 2 × kReleaseStepMs) / kBlockMs + 20` |
| `NotchControllerStereo.LinkedAutoReleaseWaitsForBothLanes` | same | same |
| `NotchControllerEvents.EveryClearPathCarriesItsReason` (AutoRelease case) | `+ 10` on `kAutoReleaseMs` | `(kReleaseFirstMs + kReleaseStepMs) / kBlockMs + 10` (Manual −12 ⇒ 40 s) |

None of the six now reports two Clears — the ladder emits Retunes between rungs and one
Clear at the bottom, which is the thing the brief warned to check.

---

## 4. Measured ladder timings

All from passing assertions; live time is the `FakeClock`, one block = 10.667 ms, so
each bound is exact to within one block.

**Detector notch at −18 (`ReleaseWalksTheLadderAt30sThen10sPerRung`):**

| Quiet elapsed | Depth |
|---|---|
| 29.00 s | −18 (still) |
| 30.50 s | **−12** |
| 39.50 s | −12 (still) |
| 41.00 s | **−6** |
| 51.50 s | **cleared** (`active == false`, `notchCount == 0`) |

**Detector notch at −24 (`ALoweredCeilingAlsoCapsTheReclampTarget`):** at 52.00 s of
quiet the notch stands at −6 with `releasedSteps == 3` — i.e. releases fired at 30 s,
40 s and 50 s, and the Clear would come at 60 s. Matches the brief's rung table.

**Freeze (`RingRiskAtRisingFreezesTheReleaseClock`):** 90.00 s of quiet at score
exactly `0.55 × 0.7` leaves `quietMsForTest == 0.0` and the depth at −18 — three times
the first rung's cost, no cap. Removing the override and pumping 31.00 s then gives
−12, so the clock resumed from 0 rather than from anything it had "owed".

**Freeze band edges:** score `0.385 − 0.01` releases at 31.00 s (−12 → −6); an
`ringRiskValid == false` reading with score 1.0 also releases at 31.00 s.

**Preset −12 (`APresetNotchReleasesDownTheLadderAndReclampsToItsOwnDepth`):** −6 at
31.00 s, still active; reclamp to −12 once the bin rings.

**Reclamp latency (`AReturningHowlReclampsImmediatelyToDeepestDb`):** from −12 with
`releasedSteps == 2` back to −24 in **under `kDeepenAfterMs` (300 ms)** of live time —
measured as `liveMsForTest()` delta across the returning howl. `releasedSteps` reads 0
afterwards, and the next release then costs the full 30 s (still −24 at +29.0 s, −18 at
+30.5 s), which is the observable proof the counter was reset.

**Ceiling latency (`LoweringTheCeilingPullsADetectorNotchUpOnTheNextTick`):** −18 → −12
within 200 ms of the slider move (≈19 ticks; the branch fires on the first one).

---

## 5. The two edges

### (a) Reclamp target equal to the current depth — **fixed, with a test**

Reachable now: climb to −24, go quiet to −12, operator sets the slider to −12. The new
per-tick ceiling clamp pulls `deepestDb` from −24 down to −12, so on the next reinforce
`target == n.depthDB` and the old code would have called `pushRetuneLocked` with a depth
change of zero — a `Set` that restarts `Biquad`'s 10 ms ramp on a live PA for nothing.

Fix (reinforce loop, in scope for this edit only): `if (target == n.depthDB ||
pushRetuneLocked (…))`. The short-circuit skips the push; the **bookkeeping still runs**
— `releasedSteps` back to 0, `deepestDb = target`, `stageChangedAtMs` stamped. Skipping
that too would have been the worse bug: the notch's next release would cost 10 s instead
of 30.

Pinned by `ACeilingLevelWithTheReleasedDepthReclampsWithoutResendingIt`, which asserts
all three things — no `Retune` event, no `Set` command on that (channel, index), and
`releasedStepsForTest == 0`.

### (b) Slider raised ABOVE `deepestDb` while released — **verified, no reason fix needed**

Sequence: climb to −24, release to −12, slider to −6. The ceiling pass in step 3 runs
*every tick*, sees `−12 < −6`, and pulls the notch to −6 under `RetuneReason::Ceiling`
— which is the correct label for a slider move — and also clamps `deepestDb` to −6. By
the time the howl returns, `target = max(−6, −6) == depthDB`, so edge (a)'s guard fires
and no `Reclamp` is emitted at all. A shallower depth under reason `Reclamp` is
therefore unreachable.

I did not need the fallback the task allowed ("run the ceiling clamp of `deepestDb`
before the reclamp compare") as a *reordering* — but I did make the ordering explicit
and load-bearing inside step 3: `n.deepestDb = std::max (…)` is written **before** the
`if (n.depthDB < ceiling)` Set, with a comment saying why. That is what guarantees the
reinforce loop can never read an outlawed `deepestDb`.

Pinned by `ACeilingRaisedAboveTheDeepestRungPullsUpAsCeilingNotReclamp`: any `Reclamp`
event at all is an `ADD_FAILURE`, and the `Ceiling` retune is asserted as −12 → −6.

---

## 6. Files

- `src/app/NotchController.cpp` — step 3 replaced; reclamp guard (edge a). +152/−17
- `src/app/NotchController.h` — `kAutoReleaseMs` doc string. +6/−2
- `src/gui/SpectrumView.h` — `kRingRiskRisingFraction` aliases
  `NotchController::kRiskFreezeFraction`. +9/−1
- `tests/test_notchcontroller.cpp` — `pumpQuietFor`, 16 new tests, 6 pump lengths. +554/−12

Nothing under `.superpowers/` is committed; `.superpowers/sdd/.gitignore` was removed
before staging (it did not exist this run, `rm -f` was still executed).

---

## 7. Self-review

- **Lock audit.** Every `lock_guard` in `runOnce` sits in its own closed scope; none is
  nested. Order in the function: `modelMutex_` (notch-list gather, inside the drain
  loop) → `snapshotMutex_` (frame publish) → `modelMutex_` (live clock) →
  `snapshotMutex_` (`releaseFrozen`) → `modelMutex_` (the ladder). Grepping the step-3
  region shows `snapshotMutex_` appears only at the publish, which **closes before**
  `if (tapAlive) { const std::lock_guard … (modelMutex_);` opens. No `model → snapshot`
  order is created.
- **Every `pushRetuneLocked` / `pushClearLocked` in step 3 is under `modelMutex_`** —
  all three call sites (Ceiling, Release, Clear) are inside that one scope.
- **`releaseFrozen` is published outside `modelMutex_`** and unconditionally, once per
  `runOnce`.
- **Every retune call site stamps `stageChangedAtMs`.** Step 3: Ceiling ✓, Release ✓.
  Reinforce loop: Deepen ✓, Reclamp ✓ (including the skipped-push path).
- **No path sends an unchanged depth.** Ceiling is guarded by `n.depthDB < ceiling`
  (strict); Release by `n.depthDB < kDepthLadderDb[0]` with `nextShallowerRungDb`
  returning a *strictly shallower* rung; Deepen by `next < n.depthDB`; Reclamp by the
  new `target == n.depthDB` short-circuit.
- **Release never skips a rung.** `nextShallowerRungDb` returns the *deepest* fixed
  rung strictly shallower than the current depth, so −24 → −18 → −12 → −6 and nothing
  in between is jumped. An off-rung depth (a −9 preset, a −10 off-rung ceiling)
  resolves to the nearest rung above it, which is the ladder's next step, not a skip.
  The Ceiling branch *can* move more than one rung at once — deliberate, invariant 3
  bounds only the deep direction, and shallower is never dangerous.
- **No clamp, NaN check or bounds check was removed.** `pushRetuneLocked` still applies
  all five validation predicates plus the −24 floor to every depth this task emits;
  `ceilingDbFor`'s NaN sentinel is untouched.
- **Verified by a fresh full run, not by inference:** `ctest -C Release` → 516/516.

---

## 8. Concerns / for the next task

1. **`releaseFrozen` does not bump `latest_.sequence`.** It is published outside the
   drain loop, so a GUI that repaints only on a sequence change will see the flag late
   (at the next real frame). Harmless for 1.2.0 — nothing draws it — but the later GUI
   or log consumer should read it alongside a frame, not poll it for edges.
2. **The freeze reads the LAST scored frame, not "now".** `frameScoreValid_` /
   `frameMaxScore_` are cleared and refilled only when the drain loop produces a block.
   With a 5 ms poll and a ~10.7 ms hop, roughly half of all `runOnce` calls reuse the
   previous frame's numbers. This is the intended reading (they describe the most recent
   frame the detector actually scored), but it means the freeze has up to one hop of
   lag, and a slot that stops producing blocks entirely keeps the last score until the
   tap goes dead.
3. **`n.lastDetectedMs` is now written but never read by the release path.** It still
   feeds lane D's `ageMs` and the reinforce loop, so it is not dead — but if a later
   task wants "time since last reinforce", `quietMs` is the field that now means it, and
   the two can differ by everything the freeze banked.
4. **`AReturningHowlReclampsImmediatelyToDeepestDb` needed a short loop, not one pump.**
   The brief's version asserted the reclamp lands on a *single* hop of returning tone.
   It does not, and it cannot: the analysis window is four hops long and tapered, so one
   hop of tone sitting at its trailing edge is not yet a peak `peakinessAt` will pass.
   That is a property of the FFT window, not of the reclamp. The test now pumps until
   the bin rings (≤ 8 blocks) and asserts the substantive claim — the depth is back at
   `deepestDb` in **under `kDeepenAfterMs`**, i.e. without waiting out the deepen gate.
   The same paragraph's `EXPECT_DOUBLE_EQ (quietMsForTest, 0.0)` was relaxed to
   `EXPECT_LE (…, kBlockMs)` for a related reason: step 3 of the *same* `runOnce` adds
   that tick's `dt` straight back on whenever the frame was not also frozen, so exact
   zero is only reachable by accident. Both changes are documented in the test body.
5. **Docs not updated.** `CLAUDE.md`'s definition-of-done item 5 asks that a change to
   DSP constants, topology or user-visible behaviour also update `docs/GIOI-THIEU.md`
   and `docs/KY-THUAT-CHONG-HU.md` in the same change. This task's file list allows only
   the four files above, so those two docs still describe 1.1.3's 30 s cliff. The
   staircase, the freeze and the live ceiling all need to reach them (and the tester
   notes) before this lane ships — flagging it here rather than silently violating the
   task's file boundary.
6. **`kAutoReleaseMs` now has two meanings in the codebase** — its own name ("how long
   the first release takes", still true) and `kReleaseFirstMs` aliasing it. The doc
   string says so, but a future reader grepping `kAutoReleaseMs` will land on a constant
   that no longer names the behaviour it is called after. Worth collapsing to
   `kReleaseFirstMs` in a later cleanup, once nothing outside lane G reads it.

---

## Fix round 1

Commit `89b7dc3` — `test(app): releaseFrozen false on a dead tap; reclamp-over-ceiling
guarantee named at its real site; slack in re-timed tests`.

### Important 1 — brief-mandated test missing: `releaseFrozen == false` when the tap is dead

`tests/test_notchcontroller.cpp`, new test `NotchControllerLadder.ReleaseFrozenGoesFalseWhenTheTapDies`
(inserted directly after `RingRiskAtRisingFreezesTheReleaseClock`, before
`InvalidRingRiskDoesNotFreezeTheClock`).

Sequence: `setRingRiskOverrideForTest` to a RISING score, one live `pump()` (tap alive) to
confirm `snap.releaseFrozen == true`, capture `quietMsForTest`, then advance `FakeClock` by
`kTapSilenceTimeoutMs + 10.0` (250 + 10 ms) **without** writing to the tap or calling `pump()`,
call `h.controller.runOnce()` directly, and assert `copySnapshot(...).releaseFrozen == false`
and `quietMsForTest` unchanged.

Verified RED by reasoning per the brief (traced, not by reverting code): the publish site
in `NotchController.cpp` is the unconditional `{ lock_guard(snapshotMutex_); latest_.releaseFrozen
= frozen; }` block that sits BEFORE `if (tapAlive)`. If that publish were moved back inside
`if (tapAlive)` (the v1-plan mistake the brief and M-6 warn about), the block that sets
`releaseFrozen = true` would never run again once the tap dies — `latest_.releaseFrozen`
would keep reading whatever it last held, which is `true`, and the new test's
`EXPECT_FALSE (snap.releaseFrozen)` would fail. Confirmed GREEN against the shipped code:

```
$ build/tests/Release/HandsFreeTests.exe --gtest_filter='NotchControllerLadder.ReleaseFrozenGoesFalseWhenTheTapDies'
[ RUN      ] NotchControllerLadder.ReleaseFrozenGoesFalseWhenTheTapDies
[       OK ] NotchControllerLadder.ReleaseFrozenGoesFalseWhenTheTapDies (0 ms)
[  PASSED  ] 1 test.
```

### Important 2 — comment at `NotchController.cpp:~629-632` named the wrong site as the guarantee

Two edits, `src/app/NotchController.cpp`:

- **Ceiling-clamp site** (was `:630-632`, the `n.deepestDb = std::max (n.deepestDb, ceiling);`
  paragraph inside the `if (n.origin == Origin::Detector)` block, step 3 of `runOnce`, now
  `:615-631`). Removed the claim that this clamp "runs BEFORE the Set below so the reclamp
  branch ... can never see a deepestDb the ceiling has already outlawed." Replaced with:
  within one `runOnce`, step 1 (detection, which contains the reclamp) runs BEFORE step 3
  (this clamp), so on the tick the slider moves, the reclamp still sees whatever `deepestDb`
  held before this line ran this tick. This clamp's real job is keeping `deepestDb` honest
  for LATER ticks and for anything reading it directly (`savePreset`, the snapshot) between
  now and the next reclamp.
- **Reclamp site** (`const double target = std::max (n.deepestDb, ceilingDbFor (n));`, now
  `:~1178`, inside the reinforce loop's `if (n.releasedSteps > 0)` branch). Added a mirror
  sentence naming this `max()` — not the step-3 clamp — as the actual PA-safety guarantee
  that a reclamp can never land past the live ceiling, precisely because step 1 runs before
  step 3 within one tick.

No behaviour changed — comments only, per the fix-round file boundary.

### Minor 1 — thin slack in re-timed tests

`tests/test_notchcontroller.cpp`, three call sites, padding changed from `+ 20` / `+ 10`
blocks to `+ 60`, each with a comment deriving the exact blocks needed:

- `NotchControllerStereo.IndepAutoReleaseIsPerLane` (line ~1226) and
  `NotchControllerStereo.LinkedAutoReleaseWaitsForBothLanes` (line ~1256): needed =
  `ceil(30000/kBlockMs) + ceil(10000/kBlockMs) + ceil(10000/kBlockMs)` = `2813 + 938 + 938`
  = `4689`; old `+ 20` gave `4707` (18 blocks of slack); new `+ 60` gives `4747` (58 blocks).
- `NotchControllerEvents.EveryClearPathCarriesItsReason` AutoRelease case (line ~1451): needed
  = `ceil(30000/kBlockMs) + ceil(10000/kBlockMs)` = `2813 + 938` = `3751`; old `+ 10` gave
  `3760` (9 blocks of slack); new `+ 60` gives `3810` (59 blocks).

### Minor 2 — tautological `EXPECT_LT` in `AReturningHowlReclampsImmediatelyToDeepestDb`

`tests/test_notchcontroller.cpp:~2864-2891`. The loop that pumps the returning howl caps at
8 blocks (~85 ms of live time), which is always under `kDeepenAfterMs` (300 ms) regardless of
whether the reclamp branch actually skipped the deepen gate — so `EXPECT_LT (liveMsForTest()
- before, kDeepenAfterMs)` could never fail even if the gate were wrongly enforced. Removed
that assertion (and the now-unused `before` capture) and replaced it with `EXPECT_LE (blocks, 4)`
— the analysis window is four hops long and tapered, so a reclamp firing on the frame (rather
than waiting) should resolve within one window's worth of pumps. The depth assertion
(`EXPECT_DOUBLE_EQ (depthDbForTest(...), -24.0)`) is unchanged.

### Verification

```
$ cmake --build build --config Release
[... exit 0 ...]

$ build/tests/Release/HandsFreeTests.exe --gtest_filter='NotchController*'
[==========] 111 tests from 14 test suites ran. (3486 ms total)
[  PASSED  ] 111 tests.

$ cd build && ctest -C Release
100% tests passed, 0 tests failed out of 517
Total Test time (real) =  42.35 sec
```

517 (up from 516 in the original task-7 report): the one new `ReleaseFrozenGoesFalseWhenTheTapDies` test.

### Files

- `src/app/NotchController.cpp` — two comment rewrites (Important 2). No behaviour change.
- `tests/test_notchcontroller.cpp` — one new test (Important 1), three padding fixes with
  derivation comments (Minor 1), one tautological-assertion fix (Minor 2).

Commit `89b7dc3`.

---

## Fix round 1 — scoped re-review

Verified 5638555 → 89b7dc3 (diff `review-5638555..89b7dc3.diff`, 17 KB) against the
real files at HEAD, not against this report's prose.

### Finding Verdicts

**I1 — `ReleaseFrozenGoesFalseWhenTheTapDies` test.** Addressed.
`tests/test_notchcontroller.cpp:2690-2726`. Sequence matches the brief exactly:
one `pump()` (writes the tap, advances `FakeClock` by `kBlockMs` ≈10.667 ms, calls
`runOnce()`) sets `lastDataMs_ = now` and confirms `snap.releaseFrozen == true`
under the RISING override; then `h.clock.advance (kTapSilenceTimeoutMs + 10.0)`
(`NotchController.h:83`, 250 ms) with **no** `tap.write` and **no** `pump()`,
followed by a direct `h.controller.runOnce()`. Traced the production logic this
exercises, `src/app/NotchController.cpp:515-568`: `tapAlive = (nowPolled -
lastDataMs_) < kTapSilenceTimeoutMs` (line 522) is computed from `clock_.nowMs()`
against the `lastDataMs_` the earlier `pump()` stamped — since nothing wrote a new
block, `lastDataMs_` never moves, so after the 260 ms advance `tapAlive` is false.
`frozen` (line 566) is `tapAlive && riskValid && riskScore >= …`, so `tapAlive ==
false` forces `frozen == false` regardless of the still-RISING override, and the
unconditional publish at line 574-577 writes `latest_.releaseFrozen = false`. The
ladder body that advances `n.quietMs` sits behind `if (tapAlive)` (line 580), so a
dead tap also leaves `quietMsForTest` unchanged — both assertions in the test are
therefore backed by the real gating, not by coincidence. This is exactly the
regression M-6 warns about (publish moved inside `if (tapAlive)` would let a stale
`true` persist forever), and the test's own RED-IF comment names that failure
mode.

**I2 — comment correction at the ceiling clamp and reclamp site.** Addressed, and
comment-only. Diff hunk 1 (`NotchController.cpp` step 3, now lines 596-644 in the
current file) replaces the retracted claim ("It runs BEFORE the Set below so the
reclamp branch … can never see a deepestDb the ceiling has already outlawed") with
a correct statement: within one `runOnce`, step 1 (the drain loop containing
`processSpectrumForDetection`, i.e. the reclamp branch) executes before step 3
(this clamp) — confirmed by reading `runOnce`'s own structure
(`NotchController.cpp:401-644`: "1. Drain…" at 403, "2. Advance the LIVE clock" at
513, "3. Release ladder" at 541). So on the tick a ceiling change and a reclamp
would coincide, step 3's clamp cannot have run yet when step 1 reads `deepestDb`.
Diff hunk 2 (reclamp site, `NotchController.cpp:1182-1189`) adds the mirror
sentence naming `const double target = std::max (n.deepestDb, ceilingDbFor (n))`
(line 1190) as the actual unconditional, always-fresh PA-safety guarantee. Grepped
the file for the old wording (`"It runs BEFORE the Set below"`) — zero matches, no
stale copy left behind. Every `+`/`-` line in both hunks is a `//` comment line;
no executable line changed (confirmed against `review-5638555..89b7dc3.diff`
lines 16-85 and the current file content at both sites).

**Minor 1 — padding `+20`/`+10` → `+60`.** Addressed, math checks out
independently. `kBlockMs = 512/48000*1000 ≈ 10.667 ms`
(`tests/test_notchcontroller.cpp:313`), `kReleaseFirstMs = kAutoReleaseMs =
30000.0`, `kReleaseStepMs = 10000.0` (`NotchController.h:83,125-126`). For the two
three-rung sites (`IndepAutoReleaseIsPerLane` line ~1234,
`LinkedAutoReleaseWaitsForBothLanes`): `(int)((30000+2*10000)/10.6667) + 60 =
(int)4687.5 + 60 = 4687 + 60 = 4747`, comfortably above the ceil-summed 4689
actually needed. For `EveryClearPathCarriesItsReason`'s two-rung site:
`(int)(40000/10.6667) + 60 = 3750 + 60 = 3810`, above the ceil-summed 3751 needed.
Both derivation comments in the file match this arithmetic.

**Minor 2 — tautological `EXPECT_LT` in `AReturningHowlReclampsImmediatelyToDeepestDb`.**
Addressed. `tests/test_notchcontroller.cpp:2929-2946`. The old assertion compared
a `liveMsForTest()` delta against `kDeepenAfterMs` (300 ms) across a loop hard-
capped at 8 blocks (~85 ms) — mathematically unable to fail even if the reclamp
had waited out the deepen gate, so it was pure decoration. Replaced with `EXPECT_LE
(blocks, 4)`, tied to the FFT window length (four tapered hops) rather than to a
bound the loop cap already guaranteed. The depth assertion
(`EXPECT_DOUBLE_EQ (depthDbForTest(...), -24.0)`) is untouched. This is a real
tightening, not a second tautology: `blocks` is data the loop actually varies,
and the fix report's ctest run (517/517) confirms the bound holds in practice.

### New Breakage in the Fix Diff

None found. The full diff touches exactly: one new test (self-contained, adds no
shared fixture changes), two comment-only hunks in `NotchController.cpp` (verified
line-by-line — every changed line is a `//` line, no brace, condition, or
statement moved), and padding/assertion edits in three pre-existing tests whose
underlying claims are unchanged. No production behavior changed in this round.

### Verdict

All findings addressed. 0 open.
