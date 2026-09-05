# Task R1 report — publish RING RISK score into SnapshotBuffer

**Status:** DONE. Commit `f8c6f19` on `claude_desk/merge-branches-subagent-c76c7f`.
**Expected level change: 0 dB.** Readout only — no placement/clear decision,
no `NotchCommand`, no `outbox_`, no filter coefficient, no `CandidateScorer`
arithmetic was changed.

## What I implemented

Three fields on `NotchController::SnapshotBuffer` (`src/app/NotchController.h:287`):

```cpp
float ringRiskScore = 0.0f;       // max post-asym candidate score of the frame
bool  ringRiskValid = false;      // detection armed AND some lane had history
float ringRiskThreshold = 0.0f;   // = CandidateScorer::kConfirmScore (A-R3)
```

- **`ringRiskScore` (A-R1, A-R8).** Recorded in `processSpectrumForDetection`
  from the *same variable* the placement test reads:
  `const float score = breakdown.score * asym;` then `frameMaxScore_ =
  std::max (frameMaxScore_, score);`, placed immediately before
  `if (score > CandidateScorer::kConfirmScore)`. Nothing is recomputed and no
  second peakiness exists. `frameMaxScore_` is a single detector-thread member,
  so the max spans every candidate of every analysed lane of the slot — one
  score per slot, no per-lane array.
- **`ringRiskValid` (A-R4).** `frameScoreValid_ = frameScoreValid_ ||
  la.scorer.hasHistory();`, evaluated after the `!detectionActive_` early
  return (so a disarmed detector can never set it) and **before** this frame's
  `commitBlock`. The "before commit" placement is deliberate: with no committed
  frame the scorer takes its documented `historyCount_ == 0` → `rNorm = 1.0`
  branch, i.e. "everything is rising by definition" — a number that is not a
  measurement of the room and must not be published as one. Both accumulators
  are cleared in `runOnce()` just before the per-lane dispatch.
- **`ringRiskThreshold` (A-R3).** Published as `CandidateScorer::kConfirmScore`
  every frame so the GUI bands against a live value instead of a literal.
  Struct default stays `0.0f` per the brief's interface block; before the first
  frame `ringRiskValid` is false anyway, so the GUI reads Unavailable.
- **New accessor** `CandidateScorer::hasHistory() const { return historyCount_ > 0; }`
  (`src/dsp/CandidateScorer.h:99`) — const, header-inline, no behaviour change.

No new lock, no new thread, no allocation on the detector thread. The two new
members are detector-thread-only (`runOnce` / `processSpectrumForDetection`);
they reach the message thread only through the existing `snapshotMutex_`.

## A-R2: which option, and what I checked

**I took the primary option — I moved the publish block below the detection
pass.** The fallback ("keep it where it is, add a second short write") was not
needed: no test and no unwind path pins the old ordering.

What actually moved, precisely:

| Block | Before | After |
|---|---|---|
| notch-list gather under `modelMutex_` (+ `lastDataMs_`) | before detection | **unchanged — still before detection** |
| `drainIteration_` tick | after publish, before dispatch | before dispatch (unchanged relative to dispatch) |
| publish under `snapshotMutex_` | before detection | **after the per-lane dispatch loop** |

Only the `snapshotMutex_` block moved — which is exactly the block the brief
identifies as "the publish block" (`NotchController.cpp:287-315` at the time of
writing). I deliberately left the `modelMutex_` notch-list gather where it was.
That matters and is not cosmetic:

- Moving the gather too would have made a notch placed **this** frame appear in
  **this** frame's snapshot, one frame earlier than today. That is an
  observable behaviour change on an existing published field, and it would
  break spec acceptance 2 in the worst way — the risk chip would go Critical in
  the same snapshot that first shows the notch, i.e. the warning would arrive
  *with* the fix instead of before it. Leaving the gather in place keeps
  `notchCount` bit-identical to the shipped behaviour, and keeps the risk score
  strictly ahead of the notch list (the default `kPersistenceBlocks = 3` gives
  at least two Critical frames before the notch is even placed).
- `spec[]` is still alive at the new publish point: `Detector::Spectrum`
  magnitudes die at the next `processLatestBlock`, which happens at the top of
  the *next* drain iteration. The detection pass calls nothing that invalidates
  it.
- No new lock nesting: `modelMutex_` (gather) and `modelMutex_` (detection's
  reinforcement / locked-frequency loops) are both released before
  `snapshotMutex_` is taken. Nothing holds two at once, before or after.
- Ordering *between* drain iterations is unchanged; the move is entirely inside
  one iteration of the `for(;;)` drain loop.

Existing tests I specifically checked against the move:
`NotchControllerSnapshot.NotchAndSpectrumShareOneInstant` (sets a notch, then
`runOnce`, expects it in that frame — still holds, the gather did not move),
`CopyIsValueSemanticsNoAliasing`, `PublishesSpectrumWithBinsAndRate`,
`NotchControllerPreset.AdoptsWithPresetOriginOnBothChannels`, and the
`copySnapshot` call sites in `test_gui_wiring.cpp`, `test_spectrumview.cpp`,
`test_notchlistpanel.cpp`. All pass unchanged; nothing was edited to make it
pass.

## TDD evidence

### RED — tests written first, before any src change

```
cmake --build build --config Release --target HandsFreeTests
```

```
tests\test_notchcontroller.cpp(1519,5): error C2039: 'ringRiskValid': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1520,5): error C2039: 'ringRiskScore': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1535,5): error C2039: 'ringRiskThreshold': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1549,5): error C2039: 'ringRiskValid': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1553,5): error C2039: 'ringRiskScore': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1553,5): error C2039: 'ringRiskThreshold': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1567,5): error C2039: 'ringRiskValid': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1568,5): error C2039: 'ringRiskScore': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1582,9): error C2039: 'ringRiskValid': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1592,5): error C2039: 'ringRiskValid': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1593,5): error C2039: 'ringRiskScore': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1609,5): error C2039: 'ringRiskValid': is not a member of 'NotchController::SnapshotBuffer'
tests\test_notchcontroller.cpp(1618,37): error C2039: 'ringRiskScore': is not a member of 'NotchController::SnapshotBuffer'
```

(deduplicated; the cascading `C2512` / `C2672` / `C2737` gtest errors on the
same lines are omitted.)

RED is a compile failure rather than an assertion failure because the
deliverable is three new fields — there is no way to write the assertion
against a field that does not exist yet. Each of the six tests names a distinct
behaviour, and each fails on a *different* field's absence.

### GREEN — focused binary

```
build/tests/Release/HandsFreeTests.exe --gtest_filter='NotchControllerRingRisk*'
```

```
[==========] Running 6 tests from 1 test suite.
[ RUN      ] NotchControllerRingRisk.InvalidBeforeAnyFrame
[       OK ] NotchControllerRingRisk.InvalidBeforeAnyFrame (1 ms)
[ RUN      ] NotchControllerRingRisk.PublishesThresholdSoTheGuiNeverHardcodesIt
[       OK ] NotchControllerRingRisk.PublishesThresholdSoTheGuiNeverHardcodesIt (0 ms)
[ RUN      ] NotchControllerRingRisk.ValidWithHistoryAndScoreCrossesThresholdOnThePlacingFrame
[       OK ] NotchControllerRingRisk.ValidWithHistoryAndScoreCrossesThresholdOnThePlacingFrame (2 ms)
[ RUN      ] NotchControllerRingRisk.NoiseOnlyFramesAreValidAndReadLow
[       OK ] NotchControllerRingRisk.NoiseOnlyFramesAreValidAndReadLow (2 ms)
[ RUN      ] NotchControllerRingRisk.DetectionDisabledPublishesInvalidNotZero
[       OK ] NotchControllerRingRisk.DetectionDisabledPublishesInvalidNotZero (3 ms)
[ RUN      ] NotchControllerRingRisk.ScoreIsMaxOverBothLanesOfTheSlot
[       OK ] NotchControllerRingRisk.ScoreIsMaxOverBothLanesOfTheSlot (6 ms)
[----------] 6 tests from NotchControllerRingRisk (15 ms total)
[  PASSED  ] 6 tests.
```

### Full build + full suite

```
cmake --build build --config Release      # exit 0: app + tools + tests
cd build && ctest -C Release
```

```
438/438 Test #438: logstats_fixture .......................   Passed    0.11 sec

100% tests passed, 0 tests failed out of 438

Total Test time (real) =  35.81 sec
```

Baseline for this wave was **432/432**; 432 + 6 new = **438/438**, zero
failures, no existing test edited. Compiler warnings are unchanged
(pre-existing C4324 in `LockFreeRingBuffer.h`, C4244 in `NotchListPanel.cpp`);
none of the four touched files emits a warning.

### What the six tests actually pin

| Test | Claim |
|---|---|
| `InvalidBeforeAnyFrame` | no frame published → `ringRiskValid == false`, score 0. GUI must read Unavailable. |
| `PublishesThresholdSoTheGuiNeverHardcodesIt` | `ringRiskThreshold == CandidateScorer::kConfirmScore` (A-R3). |
| `ValidWithHistoryAndScoreCrossesThresholdOnThePlacingFrame` | uses the existing `primeAndPlace()` harness; on the frame that emitted the `Set` pair, `ringRiskValid` is true and `ringRiskScore > ringRiskThreshold`. This is the pin that the published number IS the placement number: the placement path only fires when `score > kConfirmScore`, so a readout computed any other way would not have to satisfy this. |
| `NoiseOnlyFramesAreValidAndReadLow` | 64 blocks of the existing fixed-seed `NoiseSource` → valid true, score exactly 0, and below the 0.55×thr Low boundary (A-R3 acceptance 3). |
| `DetectionDisabledPublishesInvalidNotZero` | armed and valid, then `setDetectionActive(false)` + one more block → valid false. Pins "N/A, not a reassuring Low". |
| `ScoreIsMaxOverBothLanesOfTheSlot` | `StereoHarness`, howl on lane 1 only, noise on lane 0 → the slot's score rises above 0. A lane-0-only implementation reads 0 here (A-R8). |

## Files changed

- `src/app/NotchController.h` — 3 `SnapshotBuffer` fields; `frameMaxScore_` /
  `frameScoreValid_` detector-thread members.
- `src/app/NotchController.cpp` — publish block moved below the detection
  dispatch; accumulator clear; the two recording lines in
  `processSpectrumForDetection`.
- `src/dsp/CandidateScorer.h` — `hasHistory()` const accessor.
- `tests/test_notchcontroller.cpp` — `<algorithm>` include; 6 new
  `NotchControllerRingRisk` tests.

Not touched: `src/gui/*`, `MainComponent*` (tasks R2/R3),
`docs/spec-ring-risk.md` (A-R3 assigns the spec edit to R3).

## Self-review

- **Completeness.** All five R1 acceptance points from the task instructions
  are covered by a test. A-R1, A-R2, A-R3 (field), A-R4 and A-R8 are each
  implemented and each named in a code comment at the site that implements it.
- **YAGNI.** No per-lane arrays (A-R8 forbids), no hysteresis (that is R2 /
  A-R5), no banding enum in the DSP (A-R3 puts banding in the GUI), no new
  configuration surface. Two floats and a bool of new state.
- **Existing patterns.** `frameMaxScore_` / `frameScoreValid_` sit with
  `drainIteration_` / `linkedPlacedAt_` under the existing "detector-thread-only
  state, needs no lock or atomic" comment and follow the same lifecycle
  (cleared per drain iteration). The tests use the harnesses the amendment
  named (`Harness`, `StereoHarness`, `FakeClock`, `SineSource`, `NoiseSource`,
  `pump`, `pumpStereo`, `primeAndPlace`, `kWarmupBlocks`) — nothing new was
  invented.
- **Tests verify behaviour, not mocks.** Every test drives real audio through a
  real `LockFreeRingBuffer` into the real `Detector` / `PeakinessAnalyzer` /
  `CandidateScorer` chain. The placing-frame test is anchored to an *observed
  `NotchCommand`*, not to a number I chose.
- **Safety.** No clamp, NaN guard, denormal check or bounds check was removed.
  `score` is a product of `[0,1]`-clamped axes times a `{0.5, 1}` penalty times
  an asymmetry multiplier that is either exactly `1.0f` or the clamped bonus,
  so `std::max` cannot see a NaN or an infinity. `hasHistory()` is const and
  cannot perturb the scorer.
- **Concurrency.** Both new members are written only on the detector thread and
  read only there; they cross to the message thread inside the existing
  `snapshotMutex_` critical section. No new lock, no lock nesting, no
  allocation added to the detector thread (`std::max` on a float).
- **Pristine output.** No new compiler warnings; test output is clean.

## Concerns / notes for R2, R3 and the verifier

1. **`hasHistory()` is "ever committed", because nothing ever resets the
   scorer.** A-R4 phrases validity as "since the last reset (start / device
   change / SR change / `setWidth`)". I checked: there is currently **no** code
   path that resets a `CandidateScorer` — `setSampleRate` resets only the
   `Detector`s, and `setWidth` deliberately resets nothing (there is an
   explicit comment there saying a widen must NOT reset, with an open owner
   decision recorded for lane S). So today the two readings coincide. If a
   scorer reset is ever added, it must clear `historyCount_` (or reconstruct
   the `LaneAnalysis`) or `ringRiskValid` will lie. Worth a line in whatever
   memory note closes lane R.
2. **The LINKED skip path does not record a score for the skipped bin.** When
   LINKED and a pair was already placed over a bin this drain iteration, the
   candidate loop `continue`s before scoring, so that bin contributes nothing
   to `frameMaxScore_`. This loses nothing in practice: the lane that *did*
   place scored that same bin earlier in the same iteration, and
   `frameMaxScore_` accumulates across lanes within the iteration. Noting it so
   the verifier does not read it as a hole.
3. **`ringRiskThreshold` is a compile-time constant today.** It is published
   per frame as A-R3 requires, but it will never vary at runtime unless
   `kConfirmScore` becomes tunable. R2's `riskForScore` should still read it
   from the snapshot rather than the constant — that is the point of A-R3 — but
   nobody should expect it to track the RESPONSE preset the way the original
   spec §2 imagined.
4. **A-R7 remains open and untouched** (`CandidateScorer.cpp:51` gates on
   `PeakinessAnalyzer::kDefaultThreshold` rather than the live
   `analyzer.getThreshold()`). Out of scope for lane R; still awaiting an owner
   decision. It has a visible consequence for this readout: with a non-default
   RESPONSE preset, the score's peakiness gate does not move with the
   analyzer's, so the risk number is computed against 10.0 regardless.
5. **Not listened to on a rig.** Per CLAUDE.md's current definition of done the
   listen happens in alpha, and per A-R10 the alpha release is the
   coordinator's to run after the whole-branch review — I did not run
   `release-alpha.ps1`.
6. **`docs/spec-ring-risk.md` still carries the superseded §2 banding** and
   still says the data source is not implemented. A-R3 / A-R10 assign that edit
   to R3; I left it alone rather than half-editing a doc another task owns.


---

# Fix round 1 — A-R4 "since its last reset" made real

**Finding fixed (Important):** `ringRiskValid` survived a sample-rate / device
change. `CandidateScorer::hasHistory()` meant "ever committed since
construction", nothing ever reconstructs `LaneAnalysis`, so after a device or
SR change the chip published `true` on the very first frame while the scorer's
rise history and baseline EMAs still held old-rate magnitudes at bin indices
that now map to different frequencies — the "tran an sai" case the spec forbids.

**Expected level change: still 0 dB.** No placement or clear decision, no
`NotchCommand`, no `outbox_`, no filter coefficient, no `CandidateScorer`
arithmetic changed. The scorer itself is still NOT reset (lane D removed that
deliberately); placement behaviour is bit-identical.

## What changed

| File | Change |
|---|---|
| `src/app/NotchController.h` | `std::uint32_t blocksSinceReset = 0;` on `LaneAnalysis` (per lane), with the reasoning comment |
| `src/app/NotchController.cpp` | increment beside `la.scorer.commitBlock(...)` (saturating); zero all lanes in `setSampleRate()` and in `setWidth()`; validity now reads the counter; `#include <limits>` |
| `src/dsp/CandidateScorer.h` | `hasHistory()` **removed** — its only caller is gone, no dead accessors |
| `tests/test_notchcontroller.cpp` | 2 new `NotchControllerRingRisk` tests |

The counter lives on `LaneAnalysis`, not on the scorer, exactly because the
scorer must not be reset: this is the readout's own memory, and clearing it
changes nothing a notch depends on. It saturates at `UINT32_MAX` instead of
wrapping — a wrap to 0 would blink the chip to N/A once every ~1.4 years of
continuous running for no reason.

`frameScoreValid_` is still evaluated BEFORE this frame's `commitBlock`, so the
first frame after a reset publishes N/A and the next one publishes a
measurement. That is the behaviour the two new tests pin.

## Reset paths I found

Searched every caller of `NotchController::setSampleRate` / `setWidth` and every
`Detector` mutation in `src/app` and `src/gui`:

1. **`NotchController::setSampleRate` (`NotchController.cpp:845`)** — the only
   place a `Detector` is retuned. Zeroed. Note for the record: **nothing outside
   the tests calls it** (`grep -rn setSampleRate src` — the other hits are
   `AudioEngine`/`NotchChain`, a different object). So in the shipping app the
   controller's `Detector` keeps its constructed 48 kHz. That is a pre-existing
   gap unrelated to lane R (it predates this branch and is not a readout bug);
   flagged here so it is not mistaken for something this fix introduced.
2. **`NotchController::setWidth` (`NotchController.cpp:79`)** — zeroed
   **unconditionally**, not only when the width value changes. This is the path
   a real device or sample-rate change actually takes:
   `DevicePanel::changeDevice / changeSampleRate / changeBufferSize` all fire
   `onBeforeRestart` → `onAfterRestart` (`DevicePanel.cpp:101/136/159`), and
   `MainComponent`'s `onAfterRestart` hook calls
   `controller.setWidth (engine_.getSlotConfig (i).width)` for every slot on
   every restart (`MainComponent.cpp:316-327`), usually with the SAME width.
   A conditional zero would therefore have missed the device/SR case entirely.
   A brief N/A after a restart is honest; a stale number is not.
3. **No other reset path exists.** `Detector::reset()` has no caller in
   `src/app` or `src/gui`; the only `.reset()` hits there are
   `NotchChain::reset()` (`AudioEngine.cpp:617,711`) and
   `SessionLogger`'s stream. `setDetectionActive(false)` already forced
   `ringRiskValid` false via the early return, and is covered by the existing
   `DetectionDisabledPublishesInvalidNotZero`.

Concern 1 of the original report ("`hasHistory()` is 'ever committed', because
nothing ever resets the scorer") is now closed: validity no longer depends on
the scorer's internal counter at all.

## Covering tests

| Test | Claim |
|---|---|
| `SampleRateChangeInvalidatesUntilTheNextBlock` | warm to valid → `setSampleRate(44100)` → next published frame reads `valid == false`, `score == 0` → one more block → `valid == true` again |
| `WidthChangeInvalidatesUntilTheNextBlock` | warm to valid → `setWidth(1)` → next published frame N/A → next block valid again. Comment names it as the path `onAfterRestart` takes |

**Mutation check (the tests actually bite).** With both zeroing sites commented
out and everything else identical:

```
build/tests/Release/HandsFreeTests.exe --gtest_filter='NotchControllerRingRisk*Invalidates*'
```

```
[  FAILED  ] NotchControllerRingRisk.SampleRateChangeInvalidatesUntilTheNextBlock
[  FAILED  ] NotchControllerRingRisk.WidthChangeInvalidatesUntilTheNextBlock

 2 FAILED TESTS
```

The mutation was reverted before the final build below.

## Commands run

### Focused

```
build/tests/Release/HandsFreeTests.exe --gtest_filter='NotchControllerRingRisk*'
```

```
[==========] Running 8 tests from 1 test suite.
[       OK ] NotchControllerRingRisk.InvalidBeforeAnyFrame (0 ms)
[       OK ] NotchControllerRingRisk.PublishesThresholdSoTheGuiNeverHardcodesIt (0 ms)
[       OK ] NotchControllerRingRisk.ValidWithHistoryAndScoreCrossesThresholdOnThePlacingFrame (2 ms)
[       OK ] NotchControllerRingRisk.NoiseOnlyFramesAreValidAndReadLow (2 ms)
[       OK ] NotchControllerRingRisk.DetectionDisabledPublishesInvalidNotZero (2 ms)
[       OK ] NotchControllerRingRisk.ScoreIsMaxOverBothLanesOfTheSlot (5 ms)
[       OK ] NotchControllerRingRisk.SampleRateChangeInvalidatesUntilTheNextBlock (2 ms)
[       OK ] NotchControllerRingRisk.WidthChangeInvalidatesUntilTheNextBlock (2 ms)
[  PASSED  ] 8 tests.
```

The six original ring-risk tests are unchanged and still pass.

### Full build + full suite

```
cmake --build build --config Release
cd build && ctest -C Release
```

```
440/440 Test #440: logstats_fixture .............................................................................   Passed    0.13 sec

100% tests passed, 0 tests failed out of 440

Total Test time (real) =  43.31 sec
```

438 before this fix + 2 new = **440/440**, zero failures, no existing test
edited. No new compiler warnings (the pre-existing C4324 in
`LockFreeRingBuffer.h` and C4244 in `NotchListPanel.cpp` are the only ones).

## Residual concerns

1. **`NotchController::setSampleRate` is production-dead** (item 1 above). Lane
   R now handles it correctly if it is ever wired up, and the restart path is
   covered through `setWidth` regardless — but somebody should decide whether
   the controller's `Detector` ought to follow the device rate at all. Out of
   scope here (it would change detection behaviour); worth a ledger line.
2. Concerns 2-6 of the original report stand unchanged (LINKED skip path,
   `ringRiskThreshold` being a compile-time constant today, A-R7 still open,
   no rig listen, `docs/spec-ring-risk.md` §2 still superseded and owned by R3).
