# Task 6 report — deepen and reclamp inside the reinforce loop

Status: **DONE**. Commit `2804f94` on `claude_desk/lane-g-brainstorm-sdd-f3c568`.

## Expected level change

6 dB deeper per 300 ms of live time while the bin stays over the peakiness
threshold, each step ramped over 10 ms by `NotchChain`, until the ceiling's
rung. This restores the suppression Task 5 removed: a fast howl still reaches
−18 / −24, just ~300–600 ms later than 1.1.3. A returning howl on a notch that
had begun releasing jumps back to `deepestDb` in one step with no 300 ms wait
(branch implemented, not yet reachable — see M-A below). **0 dB everywhere
else**: nothing outside a notch's own bin moves.

## Implemented

`src/app/NotchController.cpp`, inside the reinforce loop of
`processSpectrumForDetection` (found by the anchor comment
`// Same live threshold analyse() used to accept candidates --`, now at
`:1002`; the loop's `modelMutex_` is taken at `:988`, the block closes at
`:1089`). The brief's replacement block was applied verbatim, with one
cosmetic deviation: two comment references to "Task 7" were reworded to "the
release ladder" so the source does not carry SDD task numbering. No logic
differs from the brief.

Behaviour added on a hit (`peakinessAt(bin) > threshold`), after the existing
`n.lastDetectedMs = liveMs_`:

- `n.quietMs = 0.0` — the release clock's banked quiet time is spent, not
  paused. Task 7 depends on this.
- **Reclamp** when `n.releasedSteps > 0`: target `std::max(n.deepestDb,
  ceilingDbFor(n))`, reason `Reclamp`, no time gate; on success
  `deepestDb = target`, `releasedSteps = 0`, `stageChangedAtMs = liveMs_`.
  The `max()` is the live-ceiling clamp (M-B): a notch that climbed to −24,
  released under a raised slider, then met a lowered one must not reclamp
  12 dB past its ceiling.
- **Deepen** otherwise, only when `n.origin == Origin::Detector`,
  `n.depthDB > ceiling` (still shallower than the ceiling) and
  `liveMs_ - n.stageChangedAtMs >= kDeepenAfterMs (300)`: one rung via
  `nextDeeperRungDb(n.depthDB, ceiling)`, reason `Deepen`; on success
  `deepestDb = next`, `stageChangedAtMs = liveMs_`.

Soundcheck notches are skipped by the loop's existing
`n.origin == Origin::Soundcheck` guard. Preset/Manual never deepen because
`ceilingDbFor(n)` is their own depth, so `n.depthDB > ceiling` is false — they
still reclamp, per the brief.

Header **untouched**. `NotchCommand.h`, `AudioEngine`, `MainComponent`
untouched. `git diff --stat` for the commit: two files, 340 insertions, 0
deletions.

## Tests

Nine tests appended to `tests/test_notchcontroller.cpp` (the brief's eight,
verbatim except for two comment lines de-mojibaked to ASCII, plus one fuzz
test the dispatch asked for):

| Test | Asserts |
|---|---|
| `AContinuingHowlDeepensOneRungPer300ms` | −12 → −18 → −24, each step ≥ `kDeepenAfterMs` apart |
| `DeepeningStopsAtTheCeilingRung` | ceiling −18 leaves the notch at −18 through 300 blocks |
| `TheLastStepLandsExactlyOnAnOffRungCeiling` | ceiling −13.7: final step is 1.7 dB, lands exactly on −13.7 |
| `ACeilingOfMinusSixNeverDeepens` | zero `Set` commands carrying **this** (lane, index); depth and `deepestDb` stay −6 (M-2: fresh placements on other indices are legitimate and not counted) |
| `APresetNotchNeverDeepens` | a Preset notch on the tone's own bin stays at −12 |
| `LinkedLanesStayOnTheSameRung` | every LINKED pair has equal `depthDB` and `deepestDb` (liveness probed with `activeForTest`, B-3) |
| `IndepLeavesTheOtherLaneUntouchedThroughTheWholeClimb` | nothing active on the quiet lane at any rung |
| `SoundcheckNotchesNeverDeepen` | depth unchanged from placement over 200 tone blocks |
| `NoEmittedSetEverLeavesTheValidDepthWindow` (added) | across ceilings −6 / −7.3 / −10 / −13.7 / −18 / −23.4 / −24, every emitted `Set` carries `depthDB` in [−24, 0], and at least one was emitted |

### TDD evidence

RED, before touching `NotchController.cpp` (build green, tests run):

```
[  FAILED  ] NotchControllerLadder.AContinuingHowlDeepensOneRungPer300ms
[  FAILED  ] NotchControllerLadder.DeepeningStopsAtTheCeilingRung
[  FAILED  ] NotchControllerLadder.TheLastStepLandsExactlyOnAnOffRungCeiling
 3 FAILED TESTS      (7 passed of the 10 the filter matched)
```

with, e.g. `depthDbForTest(0, slot)` `Which is: -12` against the expected
`-13.7` — exactly the three failures and exactly the reason the brief's Step 2
predicted. The other five new tests passed red-side because they assert
"nothing moves", which was trivially true before the branch existed; they are
regression guards, not drivers. The fuzz test also passed red-side (nothing
was emitting retunes yet) and only becomes load-bearing after the change.

GREEN, focused (`--gtest_filter=NotchController*`):

```
[==========] 94 tests from 14 test suites ran. (1493 ms total)
[  PASSED  ] 94 tests.
```

GREEN, full gate (`cd build && ctest -C Release`):

```
100% tests passed, 0 tests failed out of 500
Total Test time (real) =  41.00 sec
```

500 = the 491 on the branch before this task + 9 new.

### Measured deepen timings

Instrumented run of `AContinuingHowlDeepensOneRungPer300ms` (instrumentation
reverted before commit; `grep -c ADD_FAILURE` = 0 in the committed file):

```
placedAt=746.667  saw18=1056.000  saw24=1365.333
d18=309.333       d24=309.333
```

309.333 ms = 29 hops of 10.667 ms — the smallest whole number of blocks that
clears the 300 ms gate, and one block of it is the test's own post-pump read of
`liveMsForTest()`. Both intervals are identical, and −18 was observed as a
distinct state between −12 and −24, so the ladder took **exactly one rung per
gate and skipped none**.

### M-3 — the stopping behaviour is NOT tested here, and no test pretends it is

The harness writes the raw tone straight into `h.tap`; nothing applies the
notch chain between the tone and the Detector, so the analyser sees the
UN-notched spectrum every frame. In this fixture the bin never goes quiet and
the ladder always climbs to the ceiling — which is what
`AContinuingHowlDeepensOneRungPer300ms` asserts. The Q7 claim "the ladder stops
at the first rung that quiets the bin" is observable only on a real rig and
belongs in the tester notes (Task 10). No fake notched-spectrum source was
built: one that subtracts a modelled notch would test the model, not the loop.
This is stated in a comment block above the new tests as well as here.

### M-A — the reclamp branch has no red test in this task

`releasedSteps` is incremented in exactly one place, the release ladder, which
does not exist until Task 7; `retuneForTest` only forwards to
`pushRetuneLocked`, which writes `depthDB` and nothing else, so it cannot
fabricate the state. Widening that seam was rejected — a seam that can
fabricate `releasedSteps` can hide the bug where the release ladder fails to
set it. `AReturningHowlReclampsImmediatelyToDeepestDb` therefore lives in
Task 7. **This task's green run proves the deepen half only.** The reclamp
branch is written and compiles, and is currently unreachable.

## Files

- `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-g-brainstorm-sdd-f3c568\src\app\NotchController.cpp` — +76 lines in the reinforce loop
- `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-g-brainstorm-sdd-f3c568\tests\test_notchcontroller.cpp` — +264 lines, 9 tests

## Self-review

**Lock audit.** Both new `pushRetuneLocked` call sites are at `:1039`
(Reclamp) and `:1079` (Deepen). The enclosing `const
std::lock_guard<std::mutex> lock (modelMutex_)` is at `:988`; its block closes
at `:1089`. Both sites are strictly inside. No `setNotch`/`setNotchImpl` is
called from the loop, so the non-recursive-mutex deadlock (B-1) is avoided.
`snapshotMutex_` appears only at `:478` and `:1181`, neither inside the
`988–1089` region — the "never `snapshotMutex_` inside `modelMutex_`" ordering
is preserved.

**Rung audit — a deepen never skips a rung.** `nextDeeperRungDb` returns the
*shallowest* fixed rung strictly deeper than `currentDb`, capped at the
ceiling, so from a rung it can only reach the next one down. The header warns
that when `currentDb` is DEEPER than the ceiling the function returns the
ceiling — a step in the *wrong* direction. The guard `n.depthDB > ceiling`
excludes that case before the call, which is the "compare before sending" the
header demands. Measured 309.333 ms for both steps, with −18 observed between
−12 and −24, confirms it empirically.

**No path sends the same depth twice in a row (deepen).** Under the guard
`n.depthDB > ceiling`, `next` is either a fixed rung strictly below `depthDB`
or the ceiling, itself strictly below `depthDB`. So `next < n.depthDB`
strictly: never equal, never shallower. A repeated Set that would restart the
`NotchChain` ramp geometrically cannot come from this branch.

**Reclamp is idempotent per event.** On success `releasedSteps = 0`, so the
branch is not re-entered on the following frame. On a refused
`pushRetuneLocked` (validation failure) nothing is written anywhere — no
command is queued and `releasedSteps` is left above 0 to retry — so the model
and the chain never disagree.

**No allocation added.** Two stack `double`s per hit; no container touched.
The `[detect]` log at `:944` is untouched.

## Concerns

1. **One theoretical same-depth re-send exists in the reclamp branch.** If
   `releasedSteps > 0` and the live ceiling has moved to exactly the depth the
   notch is currently standing on, `target == n.depthDB` and a Set carrying an
   unchanged depth is emitted, restarting the 10 ms ramp toward the value the
   filter is already at. It is inaudible in isolation but it does violate the
   letter of "never re-send an unchanged depth". The brief specifies this
   block verbatim and the release side that creates `releasedSteps > 0` is
   Task 7's, so I did not deviate. **Flagging it for Task 7's review**: a
   `target != n.depthDB` condition on the `pushRetuneLocked` call would close
   it, and Task 7 is the first task that can write a test for it.
2. **The reclamp branch ships untested** (M-A, above, accepted by the brief
   with open eyes). Task 7 must actually carry
   `AReturningHowlReclampsImmediatelyToDeepestDb`, or this branch reaches a PA
   system with zero coverage.
3. **`ACeilingOfMinusSixNeverDeepens` is a narrow assertion by necessity**
   (M-2): the armed detector keeps placing fresh notches on the still-ringing
   lane, so the test can only count `Set` commands carrying this exact
   (lane, index). It cannot see a stray retune that landed on a different
   index. The added `NoEmittedSetEverLeavesTheValidDepthWindow` fuzz partially
   backstops this by checking every emitted `Set` at seven ceilings.
4. **Nothing was listened to.** Per the project's Definition of Done the
   listen happens in alpha, but this is a change to how deep every automatic
   notch goes: the first rig test should start at low volume, and the tester
   note must say that a howl now reaches full depth 300–600 ms later than it
   did in 1.1.3.

## Fix round 1

Three review findings addressed. No behaviour change to the shipped notch
depths or timings — the deepen branch now compares before sending (defensive,
mathematically already guaranteed by the existing guard) and the two touched
tests assert the same true things, just more often. Nothing outside
`src/app/NotchController.cpp` and `tests/test_notchcontroller.cpp` changed.

### M-1 — deepen branch compares before sending

`src/app/NotchController.cpp:1072-1090` (the `else if (n.origin ==
Origin::Detector)` deepen branch). Before, the branch trusted the guard
`n.depthDB > ceiling` alone to guarantee `nextDeeperRungDb` returns something
strictly deeper; `NotchController.h:153-165`'s contract on that function
explicitly asks callers to compare the result against `currentDb` before
sending, because AT the ceiling it returns `currentDb` unchanged and PAST the
ceiling it can return something shallower.

Added the explicit local check:

```cpp
const double next = nextDeeperRungDb (n.depthDB, ceiling);
if (next < n.depthDB
    && pushRetuneLocked (c, i, next, RetuneReason::Deepen))
```

replacing the old `if (pushRetuneLocked (c, i, next, RetuneReason::Deepen))`.
The guard `n.depthDB > ceiling` still stands above it (kept, per the finding).

**Reachable behaviour is identical.** Within the existing guard, `n.depthDB >
ceiling` always holds, so `ceiling < n.depthDB` too. `nextDeeperRungDb`
returns either a fixed rung strictly deeper than `currentDb` (by definition
`< currentDb`) or the cap `ceilingDb` — and here `ceilingDb < n.depthDB` by
the guard, so the cap case is also `< n.depthDB`. Either way `next <
n.depthDB` is already true every time the branch could fire under the old
code, so the new condition is a no-op on the reachable domain and only
matters as documentation-as-code / future-proofing if the outer guard is ever
weakened. Confirmed with the unchanged deepen tests below.

### M-4 — upper bound on the deepen gate

`tests/test_notchcontroller.cpp`, `AContinuingHowlDeepensOneRungPer300ms`
(now ~2274-2312). Added:

```cpp
EXPECT_LT (sawMinus18At - placedAt, 2.0 * NotchController::kDeepenAfterMs)
    << "the first rung fired implausibly late";
EXPECT_LT (sawMinus24At - sawMinus18At, 2.0 * NotchController::kDeepenAfterMs)
    << "the second rung fired implausibly late";
```

right after the existing `EXPECT_GE` lower-bound pair. Measured value from
the earlier instrumented run was 309.3 ms per rung against `kDeepenAfterMs =
300` and a hop of 10.667 ms; the new bound (600 ms) leaves comfortable margin
for scheduling jitter while still catching a regression that fires the gate
every other opportunity or stalls for a block or more.

### I-2 — LINKED/INDEP asserted every frame, not once at the end

`tests/test_notchcontroller.cpp`:

- `LinkedLanesStayOnTheSameRung` (now ~2413-2456): the 200-block pump loop
  now asserts, after every `pumpStereo`, that `activeForTest(0, s) ==
  activeForTest(1, s)` for every slot, and when both are active that
  `depthDbForTest` and `deepestDbForTest` agree between the two lanes. The
  original post-loop `sawAny` check is kept unchanged below it.
  RED IF: a lane climbs a rung on a different block than its partner —
  even if both eventually saturate at the ceiling and agree by block 200
  (which is exactly the gap the old end-of-loop-only assertion could not
  see).
- `IndepLeavesTheOtherLaneUntouchedThroughTheWholeClimb` (now
  ~2440-2472): the 200-block pump loop now asserts
  `EXPECT_FALSE(activeForTest(1, s))` for every slot after every block,
  in addition to the original post-loop check.
  RED IF: a stray placement or deepen appears on the quiet lane mid-climb
  and is cleared again before block 200 — the old check only looked at the
  final state and would have missed a transient.

Both tests still pass green (see below) because the real reinforce loop
genuinely keeps LINKED lanes in lockstep (both lanes reinforced from one
frame, `.cpp:990-993`) and genuinely never touches the INDEP lane's model
(the `if (! linkedNow && c != lane) continue;` gate). Reasoning for why a
broken loop would now be caught: under LINKED, if lane 1 were reinforced on a
different code path than lane 0 (e.g. a bug that read a stale `liveMs_`
snapshot for one lane only), the two lanes would diverge on the exact block
where one crosses a rung and the other has not yet — a divergence the old
"compare final state" assertion could only see if it survived, undisturbed,
for the rest of the 200-block run. Per-frame assertion turns that transient
into an immediate failure. Deliberately not manufacturing an artificial
"broken loop" build to demonstrate this red, per the dispatch's guidance —
the reasoning above is the evidence requested in place of it.

### Commands and output

Build:

```
cmake --build build --config Release
```
→ succeeded (only pre-existing `C4324` alignment-padding warnings in
`LockFreeRingBuffer.h`, unrelated to this change).

Focused filter:

```
build/tests/Release/HandsFreeTests.exe --gtest_filter=NotchController*
```
→
```
[==========] 94 tests from 14 test suites ran. (1523 ms total)
[  PASSED  ] 94 tests.
```

Full gate:

```
cd build && ctest -C Release
```
→
```
100% tests passed, 0 tests failed out of 500
Total Test time (real) =  41.97 sec
```

500 total — unchanged from the prior task-6 run (no test added or removed,
only strengthened in place).

### Concerns carried forward

Unchanged from the original report: the reclamp branch (M-A) still has no
red test in this task — it is Task 7's to cover — and concern #1 there (a
theoretical same-depth re-send in the reclamp branch when the live ceiling
exactly equals the notch's current depth) is still open for Task 7's review.
Nothing in this fix round touched the reclamp branch or its concerns.
