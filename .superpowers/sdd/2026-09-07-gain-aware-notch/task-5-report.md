# Task 5 report — `placeConfirmed` places SHALLOW (−6 dB, −12 on a steep rise)

**Status:** DONE. Commit `1f1d7e6`, branch `claude_desk/lane-g-brainstorm-sdd-f3c568`.

## Expected level change (stated up front, per CLAUDE.md)

At the notch's own bin, a detector placement is now **12 dB shallower** than
1.1.3 at the instant of placement (**6 dB** shallower when the rise is steep).
**0 dB** everywhere else in the spectrum. Task 6 walks it back down at 6 dB per
300 ms, so an explosively building howl is audibly howling up to ~0.6 s longer
than on 1.1.3. This is the row testers must try at low volume first.
Soundcheck notches are unaffected (KD-7): still the full slider depth, on the
first block.

## Implemented

`src/app/NotchController.cpp`, `placeConfirmed` only.

- `const double depthDb = notchDepthDb_.load(...)` became
  `const double ceiling = notchDepthDb_.load(...)` — the slider is now the
  CEILING (Q1), not the placement depth.
- The whole depth choice moved **below** the index lookup (B-2), so Task 8's
  room-memory lookup can be inserted inside the locked index block (a
  placement that bails on a full chain must not consume a memory entry).
  A comment marks both hook points ("Task 8 hooks the room-memory lookup in
  HERE" inside the lock; "step 3 … is inserted HERE by Task 8" in the ladder).
- Ladder: `depthDb = kDepthLadderDb[0]` (−6); `kDepthLadderDb[1]` (−12) when
  `pc.breakdown.riseRatio >= kSteepRiseRatio`; then
  `depthDb = std::max(depthDb, ceiling)` — `max` picks the SHALLOWER because
  deeper is more negative, so an off-rung ceiling of −10 is itself the deepest
  placement (Q13, `presets/Music.json`).
- KD-7 stays **last**: `if (origin == Origin::Soundcheck) depthDb = ceiling;`.

The `scored.riseRatio = pc.breakdown.riseRatio` line the brief asks for in
Step 4 was **already present** (landed with Task 4) — no change needed. The
`setNotchImpl` call and the `[detect]` log line's `depth=` field both already
read the identifier `depthDb`, which now carries the ladder value; no textual
edit was required there either.

**m-C honoured:** the replaced region started at the two `// soundcheckActive()
takes modelMutex_ itself …` comment lines. `grep -c "soundcheckActive() takes
modelMutex_ itself"` → **1**. No duplicate.

Nothing else changed. `NotchController.h`, `NotchCommand.h`, `AudioEngine`,
`MainComponent`, `SessionLogger` untouched. No clamp removed; the
`kMaxDepthDb` clamp in `setNotchImpl` still runs downstream of everything here.

## Tests

`tests/test_notchcontroller.cpp`.

**Corrected (Step 1, M-12 — these encoded the old fixed depth):**

- `NotchControllerDetection.PersistentHowlSetsNotchOnBothChannels` — `cmd.depthDB`
  `-18.0f` → `-12.0f`. `SineSource` switches a full-scale tone on in one block,
  so `riseRatio` is far past `kSteepRiseRatio` and every SineSource-driven
  detection lands on the **steep** rung, not the shallow one.
- `NotchControllerDetection.SetNotchDefaultsFlowIntoPlacedNotch` — `-24.0f` →
  `-12.0f`, plus a new `deepestDbForTest(0, slot) == -12.0` assertion. The Q
  still flows through; the depth default is now a ceiling that permits the
  whole ladder.

**New fixture (Step 2), inserted after `pump` closes (B-6):** `kRampStartAmp`,
`RampSineSource` (+9.5 dB / 250 ms, `amp` clamped ≤ 0.9), `primeAndPlaceSlowly`
(64 noise blocks + ≤ 150 ramp blocks).

**New tests, all in `NotchControllerLadder`:**

| Test | Asserts |
|---|---|
| `ASlowlyRisingHowlIsPlacedAtMinusSix` | −6 depth + deepest, `active`, and `riseRatio ∈ [1.5, 2.0)` |
| `ASteeplyRisingHowlIsPlacedAtMinusTwelve` | −12 depth + deepest on `SineSource` |
| `ACeilingOfMinusSixCapsEvenASteepRise` | ceiling −6 clamps the steep rung |
| `AnOffRungCeilingIsItselfTheDeepestPlacement` | ceiling −10 places at −10 (Q13) |
| `SoundcheckPlacesAtTheFullSliderDepth` | KD-7: −18 |
| `PresetNotchesKeepTheirOwnCeilingAndDetectorNotchesFollowTheSlider` | Q8 |

### Results

```
build/tests/Release/HandsFreeTests.exe --gtest_filter=NotchController*
  85 tests from 14 test suites ran. [  PASSED  ] 85 tests.

cd build && ctest -C Release
  100% tests passed, 0 tests failed out of 491
  Total Test time (real) =  42.64 sec
```

491 = 485 baseline + 6 new. Build warnings: only the four pre-existing C4324
"structure was padded due to alignment specifier" from
`src/dsp/LockFreeRingBuffer.h` (untouched header). **No new warnings.**

## TDD evidence (RED before GREEN)

Tests written and built first, implementation absent. Observed:

```
[  FAILED  ] NotchControllerDetection.PersistentHowlSetsNotchOnBothChannels
     cmd.depthDB  Which is: -18   vs  -12
[  FAILED  ] NotchControllerDetection.SetNotchDefaultsFlowIntoPlacedNotch
     Which is: -12   (deepestDb -18)
[  FAILED  ] NotchControllerLadder.ASlowlyRisingHowlIsPlacedAtMinusSix
     depthDbForTest   Which is: -18   vs  -6
     deepestDbForTest Which is: -18   vs  -6
[  FAILED  ] NotchControllerLadder.ASteeplyRisingHowlIsPlacedAtMinusTwelve
     depthDbForTest   Which is: -18   vs  -12
[  FAILED  ] NotchControllerLadder.PresetNotchesKeepTheirOwnCeiling…
     Expected: (slot) != (0), actual: 0 vs 0
 5 FAILED TESTS
```

`ACeilingOfMinusSixCapsEvenASteepRise`, `AnOffRungCeilingIsItselfTheDeepest…`
and `SoundcheckPlacesAtTheFullSliderDepth` passed while RED — they assert the
ceiling, and the old code placed at the ceiling by definition. They are still
worth keeping: they are the tests that go red if Task 8 or a later refactor
drops the `max(depth, ceiling)` clamp or moves the KD-7 line.

No `git stash` was used at any point.

## The tuned `kRampStartAmp` and the observed `riseRatio`

**Shipped value: `kRampStartAmp = 1.5e-4f`** — HALF the brief's derived
`3.0e-4`. **Observed `riseRatio` at confirm: `1.7507`**, `refAgeMs = 117.33`,
peakiness 105.2, score 1.0000, confirm at **tone block 16**.

### Why the derived value failed, and it is not the reason the brief predicted

The brief's failure mode for a too-high start amplitude is "an onset step ⇒ a
tone-vs-noise ratio ≫ 2.0". At `3.0e-4` the measured ratio was **2.117** — only
just over the threshold, not 64×. The instrumented run gave
`refAgeMs = 117.33`, i.e. exactly the 11th frame back the derivation predicts,
so the reference *gap* was right and the *reference frame* was wrong.

The cause is a gap in the derivation, not in the arithmetic: **the tone phase
of `primeAndPlaceSlowly` feeds no noise UNDER the ramp.** The derivation
computes the confirm block from "peakiness > 73 ⇒ the tone ≈ 37 dB over the
noise mean ⇒ ~1.0 s ⇒ block ~93", which assumes an ongoing noise floor. With
tone-only blocks the neighbourhood holds nothing but Hann leakage, so peakiness
saturates (~105) as soon as the 2048-sample window is all tone — about 4 blocks
— and `pNorm` is pinned at 1 from then on. The confirm is therefore gated only
by `rNorm` and `mNorm`, both of which cross while the 117.3 ms reference is
still the last NOISE frame. Starting *at* the floor is not enough; the tone has
to start *below* it so that no axis can cross before the reference is itself
tone. The first all-tone reference is available at block 15 (reference =
block 4).

### The sweep that settled it (Release, this machine)

| `kRampStartAmp` | `riseRatio` | verdict |
|---|---|---|
| 3.0e-4 | 2.117 | FAILS — reference still noise |
| 2.8e-4 | 1.967 | marginal, still perturbed |
| 2.5e-4 | 1.744 | passes, edge of the plateau |
| 2.2e-4 | 1.7507 | plateau, score 1.0 |
| **1.5e-4** | **1.7507** | **shipped**, score 1.0, confirm at block 16 |
| 1.0e-4 | 1.7507 | plateau, score 0.7626 |
| 5.0e-5 | 1.7507 | plateau, score 0.7597 |
| 2.0e-5 | 1.7507 | plateau, score 0.7599 |
| 1.0e-5 | 1.7507 | plateau, score 0.7874 |

From 1e-5 to 2.2e-4 the ratio is **flat at 1.7507 across more than a decade of
start amplitude**, with `refAgeMs` fixed at 117.33. That flatness is the real
evidence the fixture is measuring the ramp and not the onset: in a tone-vs-tone
comparison the ratio is a property of the SLOPE alone and cannot depend on the
start level. 1.5e-4 sits mid-plateau — 2× below the value that fails, a decade
above where the score margin over `kConfirmScore` thins to ~0.76.

1.7507 rather than the derived 1.671 because the ratio compares two 4-hop FFT
windows over an exponential ramp, which widens the effective gap slightly. It
is inside the `[1.5, 2.0)` band the test asserts, with 0.25 of headroom below
`kSteepRiseRatio`.

`kSteepRiseRatio` and `kConfirmScore` were **not** touched. The 9.5 dB/250 ms
slope was **not** touched. Only `kRampStartAmp` moved, exactly as the brief's
tuning rule directs.

The instrumentation used to measure this (`std::printf` of `riseRatio`, `rNorm`,
peakiness, score, `refAgeMs`, confirm block) was temporary and is **removed** —
`grep -n "TUNE\|printf" tests/test_notchcontroller.cpp` returns nothing. The
numbers survive in the comment above `kRampStartAmp`, so the next person to
retune has the sweep instead of having to redo it.

## Deviations from the brief

1. **`kRampStartAmp` is 1.5e-4, not 3.0e-4** — the sanctioned tunable, settled
   by running the test, with the sweep recorded in-file. Reasoning above.
2. **The fixture comment block was rewritten** to say the tone starts BELOW the
   noise floor (the brief said "AT"), to record the sweep, and to name the real
   gating mechanism. `primeAndPlaceSlowly`'s comment lost the stale "~96 where
   the derivation puts the confirm" (measured: block 16) and its stale
   amplitude figure (0.33 → 0.16 at block 150, since the start amplitude
   halved).
3. **`PresetNotchesKeepTheirOwnCeilingAndDetectorNotchesFollowTheSlider` needed
   a drain the brief did not include.** As written it fails on
   `ASSERT_NE(slot, 0)` even with the implementation in place — a real fixture
   bug, not a policy failure. The `setNotch(0, 0, …, Origin::Preset)` call
   leaves its own `Set` command in `h.commands`; the first `runOnce()` inside
   `primeAndPlace`'s warmup flushes it, and `primeAndPlace` then reads that
   command (it takes the first command available once two are queued) and
   reports the preset's index 0 as the detector's slot. The test would then be
   asserting the preset's depth against the detector's policy. Added an
   explicit `runOnce()` + drain between the preset assertions and
   `setDetectionActive(true)`, with a comment saying why. This preserves the
   test's intent exactly and is the only way it can test what it claims to.
4. **Commit message** is the one in the task instructions, not the brief's
   (they differ); ends with the required `Co-Authored-By` trailer.
5. **Step 4's `scored.riseRatio` and `setNotchImpl` edits were already in
   place** from Task 4 — no edit made, nothing to deviate from.

## Files changed

- `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-g-brainstorm-sdd-f3c568\src\app\NotchController.cpp`
- `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-g-brainstorm-sdd-f3c568\tests\test_notchcontroller.cpp`

`git diff --stat`: 2 files changed, 271 insertions(+), 4 deletions(-).
`.superpowers/sdd/.gitignore` removed before staging; `git add` used explicit
paths; nothing under `.superpowers/` was committed
(`git status` still shows `?? .superpowers/sdd/2026-09-07-gain-aware-notch/`).

## Self-review — every surviving `-18` in `tests/test_notchcontroller.cpp`

`grep -n -- "-18" tests/test_notchcontroller.cpp` → 21 hits. Each justified:

| Line(s) | Text | Why it survives |
|---|---|---|
| 529 | comment: "−18 arrives later, via the deepen path (Task 6)" | prose, and it is the correct new story |
| 1795–1796 | `nextDeeperRungDb(-12,-24) == -18`, `(-18,-24) == -24` | pure ladder arithmetic. −18 is a RUNG. Task 4's unit test, no placement involved |
| 1808–1811 | comment + `nextDeeperRungDb` against a −18 ceiling | same: rung arithmetic for the shipped `presets/Speech.json` ceiling |
| 1835–1836 | `nextShallowerRungDb(-24) == -18`, `(-18) == -12` | same, release direction |
| 1897 | `setNotch(0,0,…,-18.0, Origin::Detector)` in `CeilingIsTheSliderForDetector…` | the **public `setNotch` API**, which this task did not touch. The value is arbitrary test data; the test asserts `rawCeilingDb` is NaN and `ceilingDbFor` tracks the slider |
| 1937, 1943 | `setNotch(…,-18.0)` then `depthDbForTest == -18.0` in `ActiveForTestReadsTheFlagNotTheRetainedDepth` | public `setNotch` again; the point is that `clearNotch` must NOT zero `depthDB` (the Clear event reads it). Placement policy irrelevant |
| 2025, 2033 | `retuneForTest(0,0,-18.0, Deepen)` then `clears[0].depthDb == -18.0f` | the retune path (Task 4), not `placeConfirmed`. Asserts the Clear event carries the depth actually running |
| 2043, 2054 | `retuneForTest(0,1,-18.0,…)`, `ret->depthDb == -18.0f` | same: Retune event payload |
| 2076 | `retuneForTest(0,5,-18.0,…)` expected FALSE | guard test — slot not active. Value never applied |
| 2085 | comment: "a notch resting at −6 while the room needed −18" | prose |
| 2089, 2102 | `setNotch(…,-18.0, Origin::Detector)`, `snap.notches[0].deepestDb == -18.0f` | snapshot round-trip of `deepestDb` via the public API; Q11 (`savePreset`) |
| 2205, 2211 | `setNotchDefaults(30.0, -18.0)` then `depthDbForTest == -18.0` | **the new `SoundcheckPlacesAtTheFullSliderDepth`.** −18 here is deliberate and load-bearing: KD-7 says soundcheck takes the full slider depth |

No survivor asserts a **detector placement depth** of −18. The only two that
did (`PersistentHowlSetsNotchOnBothChannels`, `SetNotchDefaultsFlowIntoPlaced
Notch`) were corrected in Step 1.

Also checked outside this file: `tests/test_presetmanager.cpp` has three −18
hits (lines 149, 506, 963) — all preset-file *parsing* assertions against
fixture JSON, nothing to do with placement policy, all green.

## Concerns for the lane

1. **The fixture's margin is one block, structurally.** The confirm lands at
   tone block 16; the earliest block whose reference can be all-tone is 15. The
   protection is not that margin but the 1e-5…2.2e-4 plateau — anything in that
   range confirms after block 15. Still, a future change to `kWarmupBlocks`,
   `kPersistenceBlocks`, `kDefaultRiseReferenceMs`, `Detector::kHopSize` or the
   peakiness threshold could move the confirm back inside the noise-reference
   window, and the symptom would be `ASlowlyRisingHowlIsPlacedAtMinusSix`
   reporting −12 with `riseRatio` a little over 2. The assertion message points
   at `kRampStartAmp`, which is the right knob, and the in-file sweep table
   gives the next person the plateau without re-deriving it.
2. **`primeAndPlaceSlowly` feeds no noise under the ramp**, so `pNorm` is pinned
   at 1 from block ~4 and the test exercises the rise/novelty axes only. Mixing
   `NoiseSource` under `RampSineSource` would make the fixture match the brief's
   derivation literally (confirm at ~block 93 on peakiness) and would widen the
   usable `kRampStartAmp` range considerably. I did not do it because the brief
   specifies `primeAndPlaceSlowly`'s body verbatim and names `kRampStartAmp` as
   the tunable, and Task 7 is documented as extending this same helper region.
   Worth reconsidering if a later task finds this fixture brittle.
3. **Not listened to.** Per the 2026-08-27 owner decision the listen happens in
   alpha; the level change is stated above and belongs in the tester note. The
   audible consequence — an explosive howl ringing up to ~0.6 s longer than
   1.1.3 — is not observable from a green suite and is the row to try quietly
   first.
4. **Three of the six new tests were green while RED.** Noted in the TDD
   section above with the reason they earn their place anyway.
5. **Docs not updated.** The task restricted the change to two files, so
   `docs/GIOI-THIEU.md` and `docs/KY-THUAT-CHONG-HU.md` (CLAUDE.md DoD item 5)
   still describe the fixed −18 placement. This is user-visible behaviour and
   must be closed before the lane ships to alpha.

## Fix round 1

Comment/diagnostic-text only, per the reviewer's three findings. No token in
either file's code was touched — only string literals and `//` comments.

**Important 1 — the two diagnostic messages on
`ASlowlyRisingHowlIsPlacedAtMinusSix` pointed the wrong repair direction.**

- `tests/test_notchcontroller.cpp:2120-2122` (the `ASSERT_GE (slot, 0)` message,
  fires when nothing confirmed inside 150 blocks): was "LOWER kRampStartAmp so
  the tone spends longer under the peakiness threshold" — backwards, because
  `pNorm` is pinned at 1 from block ~4 (the file's own comment, ~:372-375), so
  lowering the start amplitude only delays the confirm further. Rewritten to
  "RAISE kRampStartAmp (still below the noise floor) or steepen gainDbPerMs so
  peakiness clears the confirm threshold sooner (B-5)".
- `tests/test_notchcontroller.cpp:2148-2151` (the `EXPECT_LT (set->riseRatio, …)`
  message, fires when `riseRatio >= kSteepRiseRatio`): said "raise
  kRampStartAmp's derivation" — the measured fix for exactly this failure
  (recorded a few lines above, in the in-file sweep) was to HALVE the value
  (3.0e-4 → 1.5e-4); raising it walks back into the failing region. Rewritten
  to state the real cause (confirm landed while the 117 ms-old reference frame
  was still a noise frame → tone-vs-noise, not tone-vs-tone) and the correct
  fix: LOWER `kRampStartAmp`, or add a few tone-only priming blocks before the
  reference ages past the onset; explicitly notes raising it is wrong.
- Checked the `RED IF` comment above the test (`:2111-2113`) and the fixture's
  own derivation comment block (`:351-390`) for the same wrong-direction
  phrasing — neither repeats it, so no further edit was needed there.

**Important 2 — `src/app/NotchController.cpp:838` (now ~:838-843) overclaimed
what moving the KD-7 line does.** It said "Moving or dropping it reds
SoundcheckPlacesAtTheFullSliderDepth" — true for dropping, false for moving:
relocating the `if (origin == Origin::Soundcheck) depthDb = ceiling;` line
above the `std::max (depthDb, ceiling)` call is a no-op today because
`max(ceiling, ceiling) == ceiling`, so no currently-passing test would catch
that reordering. Rewritten to say dropping it reds the two tests TODAY, and
that moving it above the `std::max` does not — until Task 8 inserts its
room-memory step between them, at which point the ordering starts mattering
and becomes test-visible.

**Minor 4 — `tests/test_notchcontroller.cpp:383-385` (now ~:382-392)
misattributed the measured 1.7507 to FFT window widening.** For a geometric
ramp the Hann window's weighting cancels exactly, so a 4-hop window cannot
widen the ratio. The real arithmetic: with per-hop gain
`r = 10^(0.4053/20) = 1.04778` (9.5 dB/250 ms over a 10.667 ms hop),
`r^11 = 1.6708` — the brief's derived 1.671, for an 11-hop gap matching
`refAgeMs = 117.33 = 11 × 10.667 ms`. But `r^12 = 1.75065`, which is exactly
the measured value. So the observed ratio is TWELVE hops of ramp gain compared
against an ELEVEN-hop-old timestamp — a one-hop offset between the history
entry's age label and the window content it's compared against, not a
windowing artifact. Rewrote the comment to state this with the arithmetic.

### Commands and output

```
cmake --build build --config Release
  ... NotchController.cpp, test_notchcontroller.cpp recompiled ...
  HandsFree.vcxproj -> ...\Release\AZ Soundtech Hands-free.exe
  HandsFreeTests.vcxproj -> ...\build\tests\Release\HandsFreeTests.exe
  (only the four pre-existing C4324 alignment-padding warnings; no new warnings)

build/tests/Release/HandsFreeTests.exe --gtest_filter=NotchController*
  [==========] 85 tests from 14 test suites ran. (1430 ms total)
  [  PASSED  ] 85 tests.

cd build && ctest -C Release
  100% tests passed, 0 tests failed out of 491
  Total Test time (real) =  43.84 sec
```

### Commit

`rm -f .superpowers/sdd/.gitignore` run before staging.
`git add tests/test_notchcontroller.cpp src/app/NotchController.cpp` — staged
set verified via `git diff --cached --name-only` to be exactly those two
files.

Commit `2a1c2f8`:
```
docs(app): slow-rise fixture diagnostics point the measured way; Soundcheck-last comment honest until Task 8

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
```
`2 files changed, 25 insertions(+), 11 deletions(-)` — comment/string-only, no
assertion values or code tokens changed.
