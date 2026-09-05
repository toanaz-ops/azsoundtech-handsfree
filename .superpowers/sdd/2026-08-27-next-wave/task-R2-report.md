# Task R2 report — Map score → state in the GUI, with hysteresis (lane R)

**Commit:** `7fe7636` feat(gui): map ring-risk score to a state, with a 750 ms anti-flicker hold
**Branch:** `claude_desk/merge-branches-subagent-c76c7f` (worktree
`.claude/worktrees/chore-infra-prune-orphans-948d0e`), on top of `bbc1cfb`.
**Level change:** 0 dB. Readout only — no file under `src/app/`, `src/dsp/`,
or `MainComponent*` was touched (`git show --numstat 7fe7636` lists exactly
three files).

## What was implemented

### 1. `SpectrumView::riskForScore()` — pure banding (`src/gui/SpectrumView.h:252`, `src/gui/SpectrumView.cpp:646-676`)

`static RingRisk riskForScore (const NotchController::SnapshotBuffer&)`. It
reads the three task-R1 fields and nothing else, which is what makes it
testable on a hand-filled buffer with no harness.

Bands against `snapshot.ringRiskThreshold` (ruling **A-R3**), never a
hardcoded `0.7` and never the pre-amendment `10.0`:

| Condition | State |
|---|---|
| `! ringRiskValid` | `Unavailable` |
| `score < 0.55 × thr` | `Low` |
| `0.55 × thr ≤ score < thr` | `Rising` |
| `score ≥ thr` | `Critical` |

`kRingRiskRisingFraction = 0.55f` at `SpectrumView.h:233` is the only GUI-side
number; the line it is a fraction OF comes from the detector, so the banding
tracks the RESPONSE preset instead of drifting from it.

Three additional cases report `Unavailable` rather than guess — all three are
guards, none of them removes an existing one:

- `! ringRiskValid` (spec): 0.0 here means *no number*, not *quiet room*.
- **NaN score**: every comparison in the chain is false for NaN, so without
  the guard it would fall through the two `<` branches and land on
  `Critical` — a false alarm on garbage. Commented at the definition.
- **Non-finite or `≤ 0` threshold**: against `thr = 0`, *every* score reads
  `Critical`. An honest `N/A` beats a chip permanently crying wolf. This one
  is not in the spec text; it is the same "never reassure or alarm wrongly"
  principle applied to the other end of the input.

### 2. `SpectrumView::RingRiskHysteresis` — the HOLD (`SpectrumView.h:276-296`, `SpectrumView.cpp:678-728`)

Spec §3 says "pick one and note it in the code". **Picked: the hold** (ruling
**A-R5**), with `kHoldMs = 750.0` and the reasoning written at the definition
(`SpectrumView.h:254-296`). Why hold and not split thresholds: the bands are
already fractions of a *live* threshold, so split fall-fractions would be a
second set of numbers to keep in step with the DSP; the hold is one number,
and it is the one that matches what the chip is for — a soundman who glanced
away for half a second still sees that the room just went Critical.

Rules, all covered by tests:

- **Step UP is immediate**, and it (re)starts the hold.
- **Step DOWN is blocked** while `nowMs - stepUpMs_ < 750`. Once the hold
  expires it falls straight to the raw band (the hold already absorbed the
  flicker; stepping one band at a time would just make the fall slower).
- **`Unavailable` overrides an in-flight hold.** Holding a stale `Critical`
  after the detector stopped scoring would be inventing data.

`timerCallback` (`SpectrumView.cpp:730-746`) now feeds the provider's raw band
through `ringRiskHold_` before it reaches `ringRisk_`. `ringRiskProvider`
itself is untouched and still null — R3 wires it, so nothing on screen changes
yet (see "Screenshot" in Concerns).

### Clock injection: the pattern followed, and why

**Time is a parameter, not a clock the object owns**: `apply (RingRisk raw,
double nowMs)`. That is the pattern already in *this* class —
`SpectrumView::ageMsOf (const SnapshotNotch&, double nowMs)`, whose caller
passes `juce::Time::getMillisecondCounterHiRes()` (`SpectrumView.cpp:560`,
`:993` before this change). `timerCallback` does the same.

Considered and rejected:

- **`ClockFn = std::function<double()>` ctor parameter**, as `NotchListPanel`
  takes (`src/gui/NotchListPanel.h:58`, ctor `NotchListPanel.cpp:22-30`). It
  is the right shape *there* because the panel's own timer computes ages deep
  inside a refresh with no caller to pass a time down. Here the only caller is
  one line in `timerCallback`, so a `std::function` would add an indirect call
  (and a possible heap allocation) on the message thread to buy nothing.
- **`ClockSource` / `FakeClock`** (`src/dsp/ClockSource.h`,
  `tests/test_notchcontroller.cpp:11`). That is the DSP-side interface, whose
  own header notes it exists *because* `std::function` may allocate on the
  audio thread. Nothing in the GUI implements it, and injecting a virtual
  clock into a plain value type would be a new pattern in this codebase, not
  the nearest existing one.

The parameter form is also strictly the most testable of the three: the tests
drive a 750 ms rule with plain numbers, in 0 ms, with no sleep and no
component.

## Tests (TDD)

Seven new tests in `tests/test_spectrumview.cpp:576-712`, all written and
compiled **before** any implementation existed.

### RED

```
$ cmake --build build --config Release --target HandsFreeTests
test_spectrumview.cpp(583,5): error C2039: 'riskForScore': is not a member of 'gui::SpectrumView'
test_spectrumview.cpp(583,5): error C3861: 'riskForScore': identifier not found
test_spectrumview.cpp(585,5): error C2039: 'riskForScore': is not a member of 'gui::SpectrumView'
test_spectrumview.cpp(585,5): error C3861: 'riskForScore': identifier not found
...
test_spectrumview.cpp(709,5): error C2065: 'hold': undeclared identifier
```

(`hold` is undeclared because `gui::SpectrumView::RingRiskHysteresis` did not
exist yet. A new-API RED in C++ is a compile failure; the assertions
themselves are exercised in the GREEN run below.)

### GREEN

```
$ cmake --build build --config Release
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter=*RingRisk*
[==========] Running 16 tests from 3 test suites.
[----------] 1 test from MainComponent
[ RUN      ] MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt
[       OK ] MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt (610 ms)
[----------] 7 tests from SpectrumView
[ RUN      ] SpectrumView.RingRiskBandsAgainstThePublishedThresholdNotAHardcodedOne
[       OK ] SpectrumView.RingRiskBandsAgainstThePublishedThresholdNotAHardcodedOne (0 ms)
[ RUN      ] SpectrumView.RingRiskIsUnavailableWheneverTheNumberCannotBeTrusted
[       OK ] SpectrumView.RingRiskIsUnavailableWheneverTheNumberCannotBeTrusted (0 ms)
[ RUN      ] SpectrumView.RingRiskStepsUpImmediately
[       OK ] SpectrumView.RingRiskStepsUpImmediately (0 ms)
[ RUN      ] SpectrumView.RingRiskHoldsAStepDownForTheHoldTimeThenFalls
[       OK ] SpectrumView.RingRiskHoldsAStepDownForTheHoldTimeThenFalls (0 ms)
[ RUN      ] SpectrumView.RingRiskDoesNotFlickerWhileAScoreOscillatesAcrossABoundary
[       OK ] SpectrumView.RingRiskDoesNotFlickerWhileAScoreOscillatesAcrossABoundary (0 ms)
[ RUN      ] SpectrumView.RingRiskUnavailableOverridesTheHold
[       OK ] SpectrumView.RingRiskUnavailableOverridesTheHold (0 ms)
[ RUN      ] SpectrumView.RingRiskStepUpRestartsTheHold
[       OK ] SpectrumView.RingRiskStepUpRestartsTheHold (0 ms)
[----------] 8 tests from NotchControllerRingRisk  (task R1, all OK)
[==========] 16 tests from 3 test suites ran. (229 ms total)
[  PASSED  ] 16 tests.
```

What each test pins:

| Test (`tests/test_spectrumview.cpp`) | Claim |
|---|---|
| `RingRiskBandsAgainstThePublishedThresholdNotAHardcodedOne` `:576` | All four bands at `thr = 0.42` (**non-default**) plus both exact boundaries: `score == 0.55×thr` → `Rising`, `score == thr` → `Critical`. Two rows are chosen so a hardcoded 0.7 gives the *wrong* answer (0.3 at thr 0.42 → Rising, not Low; 0.5 at thr 0.42 → Critical, not Rising). Repeated at `thr = 0.7` to show the same code path tracks a different published line. |
| `RingRiskIsUnavailableWheneverTheNumberCannotBeTrusted` `:612` | `!valid` (score 0.0 **and** 0.95), NaN score, and threshold 0 / −1 / NaN. |
| `RingRiskStepsUpImmediately` `:638` | Low → Critical in the same millisecond. |
| `RingRiskHoldsAStepDownForTheHoldTimeThenFalls` `:648` | Held at Critical at +1, +400, +749 ms; falls at exactly +750. |
| `RingRiskDoesNotFlickerWhileAScoreOscillatesAcrossABoundary` `:664` | 21 frames at 30 fps alternating Rising/Critical on the band edge → not one state change (spec §Acceptance 4), and asserts the run really stayed inside the hold. |
| `RingRiskUnavailableOverridesTheHold` `:683` | Invalid mid-hold → `Unavailable` immediately; and back to Low immediately after. |
| `RingRiskStepUpRestartsTheHold` `:697` | Hold from t=100; step up at 700 restarts it; the drop that the old hold would have allowed at 850 is still held, and lands at 1450. |

`MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt`
(`tests/test_gui_wiring.cpp:771`) was **not edited** and still passes: the
provider is null → raw `Unavailable` → the hold returns `Unavailable`.

### Full suite

```
$ cd build && ctest -C Release
...
447/447 Test #447: logstats_fixture ...............................  Passed  2.48 sec

100% tests passed, 0 tests failed out of 447

Total Test time (real) = 104.98 sec
```

Baseline before this task was 440/440; 440 + 7 new = 447. (The brief's
`432/432` figure predates lane R's own task R1, which added 8.) The suite was
run again after the self-review cleanups below: still **447/447**, 100.09 s.

## Files changed

- `src/gui/SpectrumView.h` (+84 / −9) — `kRingRiskRisingFraction`,
  `riskForScore` declaration, `RingRiskHysteresis`, the `ringRiskHold_`
  member, and a correction to the stale block above the enum.
- `src/gui/SpectrumView.cpp` (+87 / −3) — `riskForScore`,
  `RingRiskHysteresis::severity` / `::apply`, one changed line plus comment in
  `timerCallback`.
- `tests/test_spectrumview.cpp` (+164) — seven tests and a `<limits>` include.

## Self-review

- **Completeness.** Every item of the R2 brief and every R2-binding ruling is
  covered: A-R3 (bands against the published threshold, proven by a
  non-default-threshold test), A-R5 (hold chosen, 750 ms, noted in code with
  its reasoning, step-up immediate, `Unavailable` overrides), A-R9 (real line
  numbers throughout this report). The provider hook is untouched, as R3 needs
  it.
- **YAGNI.** Dropped a `state()` accessor I had first written on
  `RingRiskHysteresis` — `apply()` already returns the state and nothing else
  needed it. No new dependency, no new file, no test harness (the brief
  predicted none would be needed, and none was).
- **Existing patterns.** Time-as-a-parameter mirrors `ageMsOf` in the same
  class; the static-helpers-beside-the-enum shape matches how `ringRiskLabel`
  / `ringRiskColour` already sit; test naming and the
  comment-what-it-proves style follow the rest of `test_spectrumview.cpp`.
- **Tests assert behaviour, not implementation.** They call the public
  `riskForScore` / `apply` with values and times and check the state that
  comes back. The one place a constant is restated (`0.55f * thr`) is backed
  by two independent rows that fail for any other fraction.
- **Bounds / NaN.** Two guards *added* (NaN score, unusable threshold); none
  removed anywhere.
- **Pristine output.** Build produced no new warnings for these files; the
  focused and full runs above are the complete output.
- **Stale comment fixed.** The block above `enum class RingRisk` still read
  "THE DATA SOURCE DOES NOT EXIST YET… nothing that scores how close the room
  is to howling", which task R1 made false one commit earlier. Rewritten to
  say what is true now (score published by R1; provider still null; R3 wires
  it). Left in this commit deliberately rather than as drive-by noise — it is
  the header block that documents the very function this task adds.

## Concerns

1. **No screenshot, and I believe none is owed yet.** The project's standing
   rule is that GUI work ships a rendered picture. Nothing this commit does
   changes a pixel: the paint path is untouched, `ringRiskProvider` is still
   null, so the chip renders the identical hollow `N/A` it did before. The
   first frame where the chip shows a live state is **R3's** wiring commit,
   and that is where the screenshot is genuinely owed — ideally one per state
   (`N/A` / `LOW` / `RISING` / `CRITICAL`), since
   `ringRiskColour` / `ringRiskLabel` have never been seen rendered in
   anything but `Unavailable`.
2. **The threshold guard is mine, not the spec's.** Reporting `Unavailable`
   for a non-finite or `≤ 0` `ringRiskThreshold` is not in
   `docs/spec-ring-risk.md`. It cannot fire against today's R1 publisher
   (which writes `CandidateScorer::kConfirmScore`), and R1's own test
   `PublishesThresholdSoTheGuiNeverHardcodesIt` pins that. If a future
   RESPONSE preset ever published 0 deliberately to mean "never alarm", this
   would read `N/A` instead — which I judged the safer of the two readings,
   but it is a decision worth a reviewer's eye.
3. **Falls straight to the raw band when the hold expires.** After 750 ms a
   `Critical → Low` drop lands on `Low`, not on `Rising` first. That is what
   the brief's acceptance list describes ("after ≥750 ms it falls") and it is
   what the tests pin, but it does mean a single frame of `Low` is possible
   right at expiry if the score is still oscillating. In practice the 750 ms
   hold has already covered ~22 frames of dither, so this is the intended
   trade rather than a gap — flagged only so R3's live-rig check knows what to
   expect.
4. **`riskForScore` was verified against hand-filled buffers, not a live
   detector.** The end-to-end claim of spec §Acceptance 2 (chip goes
   `Critical` *before* the notch appears) needs R3's wiring and a real rig; R1
   already proved the DSP half of it
   (`NotchControllerRingRisk.ValidWithHistoryAndScoreCrossesThresholdOnThePlacingFrame`).
5. **Spec §2/§3 still carry the old text.** `docs/spec-ring-risk.md` still
   says "do not hardcode 10.0" and still leaves the hysteresis choice open.
   Per the amendment block, **R3** updates the spec — noting it here so it is
   not lost.

---

## Fix round 1 — both reviewer findings implemented

**Commit:** `3b8a4d4` fix(gui): re-arm the ring-risk hold on every confirming
frame, and drop it on a slot switch (on top of `f17a46b`).
**Level change:** still 0 dB — three files, all GUI/test
(`git show --numstat 3b8a4d4`: `src/gui/SpectrumView.h` +13,
`src/gui/SpectrumView.cpp` +13/−2, `tests/test_spectrumview.cpp` +92).

### What changed

**Finding 1 — the hold re-armed only on a strict step UP.**
`SpectrumView.cpp` `RingRiskHysteresis::apply` now takes the branch on
`rawSeverity >= heldSeverity` instead of `>`. An equal-severity frame changes
nothing on screen, but it *is* the level being observed again, so it refreshes
`stepUpMs_`. The hold therefore runs from the last **observation at or above**
the held state rather than from the last **change**, which is what the
reviewer's failure scenario turned on. Every A-R5 rule survives unchanged:
step up immediate, step down blocked for 750 ms, `Unavailable` overriding.
The comment at the branch spells out the failure mode it prevents.

**Finding 2 — the hold survived a slot switch.** `setController` now clears
`ringRiskHold_ = {}` alongside `firstSeenMs_` / `snapshot_` / the sequence
counters / `displayLane_`, with a comment saying why it belongs in that list.

**Comment at the `RingRiskHysteresis` definition** (`SpectrumView.h`) gained a
paragraph stating the rule in the controller's words: the hold runs from the
last time the level was *observed at or above* the held state, not from the
last time the state changed, and why arming on a strict step up would blink.

**One test hook added:** `ringRiskHoldForTest()` returns a reference to the
live `ringRiskHold_`, so the slot-switch reset is observable through the same
instance the component uses. Same `...ForTest` pattern as
`spectrumPointForTest` / `getLaneGroupForTest` / `snapshotNotchCountForTest`.

### Covering tests (3 new, `tests/test_spectrumview.cpp:718-802`)

| Test | Claim |
|---|---|
| `RingRiskDoesNotFlickerAcrossManyHoldWindows` `:731` | 90 frames (3 s at 30 fps) of a score alternating Rising/Critical on the band edge → **zero** state changes, final state Critical, plus `EXPECT_GT (now - 100.0, 4 × kHoldMs)` so the zero cannot be the old test's claim renamed. |
| `RingRiskFallsSevenFiftyAfterTheLastCriticalObservation` `:757` | The flip side: confirming the level does not make the hold immortal. Critical re-observed 1000→2000 ms, then a genuine sustained Rising — still Critical at 2500 and 2749 (750 ms after the *first* Critical, 1750, is much too early), falls at exactly 2750. |
| `RingRiskHoldDoesNotSurviveASlotSwitch` `:788` | Slot A holds Critical (raw Low at +100 still reads Critical), `view.setController (slotB.controller)`, next raw Low reads **Low** immediately. Uses the existing `FedController` harness. |

### Mutation check — all three RED against the pre-fix code

Built with the tests added but `apply` still on `>` and `setController` not
clearing the hold:

```
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter=*RingRiskDoesNotFlickerAcrossManyHoldWindows*
[ RUN      ] SpectrumView.RingRiskDoesNotFlickerAcrossManyHoldWindows
tests\test_spectrumview.cpp(746): error: Expected equality of these values:
  changes
    Which is: 6
  0
the chip changed state while the score sat on the band edge
[  FAILED  ] SpectrumView.RingRiskDoesNotFlickerAcrossManyHoldWindows (0 ms)
```

Six changes over three seconds = three blinks down-and-back, one per expired
hold window — precisely the reviewer's predicted failure, measured.

```
[ RUN      ] SpectrumView.RingRiskFallsSevenFiftyAfterTheLastCriticalObservation
tests\test_spectrumview.cpp(770): error: Expected equality of these values:
  hold.apply (Risk::Rising, 2033.0)
    Which is: 4-byte object <02-00 00-00>     (Rising)
  Risk::Critical
    Which is: 4-byte object <03-00 00-00>
  ... same at 2500.0 and 2749.0
[  FAILED  ] SpectrumView.RingRiskFallsSevenFiftyAfterTheLastCriticalObservation (0 ms)

[ RUN      ] SpectrumView.RingRiskHoldDoesNotSurviveASlotSwitch
tests\test_spectrumview.cpp(801): error: Expected equality of these values:
  view.ringRiskHoldForTest().apply (Risk::Low, 1101.0)
    Which is: 4-byte object <03-00 00-00>     (Critical — slot A's alarm)
  Risk::Low
    Which is: 4-byte object <01-00 00-00>
[  FAILED  ] SpectrumView.RingRiskHoldDoesNotSurviveASlotSwitch (30 ms)

[  FAILED  ] 3 tests, listed below:
[  FAILED  ] SpectrumView.RingRiskDoesNotFlickerAcrossManyHoldWindows
[  FAILED  ] SpectrumView.RingRiskFallsSevenFiftyAfterTheLastCriticalObservation
[  FAILED  ] SpectrumView.RingRiskHoldDoesNotSurviveASlotSwitch
```

### GREEN — focused run after the fix

```
$ cmake --build build --config Release
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter=*RingRisk*
[----------] 10 tests from SpectrumView
[       OK ] SpectrumView.RingRiskBandsAgainstThePublishedThresholdNotAHardcodedOne (0 ms)
[       OK ] SpectrumView.RingRiskIsUnavailableWheneverTheNumberCannotBeTrusted (0 ms)
[       OK ] SpectrumView.RingRiskStepsUpImmediately (0 ms)
[       OK ] SpectrumView.RingRiskHoldsAStepDownForTheHoldTimeThenFalls (0 ms)
[       OK ] SpectrumView.RingRiskDoesNotFlickerWhileAScoreOscillatesAcrossABoundary (0 ms)
[       OK ] SpectrumView.RingRiskUnavailableOverridesTheHold (0 ms)
[       OK ] SpectrumView.RingRiskStepUpRestartsTheHold (0 ms)
[       OK ] SpectrumView.RingRiskDoesNotFlickerAcrossManyHoldWindows (0 ms)
[       OK ] SpectrumView.RingRiskFallsSevenFiftyAfterTheLastCriticalObservation (0 ms)
[       OK ] SpectrumView.RingRiskHoldDoesNotSurviveASlotSwitch (11 ms)
[----------] 8 tests from NotchControllerRingRisk  (task R1, all OK)
[  PASSED  ] 19 tests.
```

The seven round-0 tests still pass unmodified — including
`MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt`
(`tests/test_gui_wiring.cpp:771`), which was not edited.

### Full suite

```
$ cd build && ctest -C Release
450/450 Test #450: logstats_fixture .............................  Passed  0.12 sec

100% tests passed, 0 tests failed out of 450

Total Test time (real) =  32.97 sec
```

447 (round-0 baseline) + 3 new = 450. Build produced no new warnings for the
touched files (the one C4996 in `MainComponent.cpp:1265` predates this lane).

### Notes for the next reviewer

- **`Unavailable` still resets `stepUpMs_` to `nowMs`.** That is unchanged
  from round 0 and harmless: `state_` becomes `Unavailable` (severity 0), so
  the next real band is a step UP and re-arms anyway.
- **The `>=` branch assigns `state_ = raw` on an equal-severity frame.**
  `severity` is a bijection over the three banded states, so equal severity
  means `raw == state_`; the assignment is a no-op kept for symmetry with the
  step-up path.
- **Concerns 1–5 of the round-0 report still stand as written** — in
  particular no screenshot is owed until R3 wires the provider and the chip
  first renders a live state.

---

## Fix round 2 — the one residual finding, closed

**Commit:** `910b1a0` fix(gui): reset the displayed ring-risk field on a slot
switch, not just the hold.
**Level change:** still 0 dB — three files, all GUI/test
(`git show --numstat 910b1a0`: `src/gui/SpectrumView.h` +7, `src/gui/SpectrumView.cpp`
+2/−1, `tests/test_spectrumview.cpp` +34/−1).

### The finding

Round 1 fixed `ringRiskHold_` (the hysteresis state) on `setController` but
missed `ringRisk_` — the **separate** field `paint()` (`SpectrumView.cpp:908-925`)
actually reads. `timerCallback()` is the only other writer of `ringRisk_`, so
between a slot switch and the next timer tick, `setController`'s own
unconditional `repaint()` (`:547`, now `:552`) could paint the *old* slot's
stale `Critical` badge for up to one 30 fps frame. The round-1 test drove
`ringRiskHoldForTest()` directly and never read `ringRisk_` / `getRingRisk()`,
so this was unverified.

### What changed

**`SpectrumView::setController`** (`src/gui/SpectrumView.cpp:541-548`) now
resets `ringRisk_ = RingRisk::Unavailable;` on the line immediately after
`ringRiskHold_ = {};`, with the comment explaining why both are needed: the
hold reset alone doesn't stop the stale value already sitting in `ringRisk_`
from being painted before the next `timerCallback()` runs.

**New test-only accessor**, mirroring the existing `ringRiskHoldForTest()`
pattern exactly: `[[nodiscard]] RingRisk& ringRiskForTest() { return
ringRisk_; }` (`src/gui/SpectrumView.h`, next to `ringRiskHoldForTest()`).
Read/write, because a test needs to *put* the displayed field into a known
state (mimicking what a live `timerCallback()` tick would have left there)
before exercising `setController()`, then read it back afterward.

### Covering test (`tests/test_spectrumview.cpp:804-831`)

`RingRiskDisplayedFieldDoesNotSurviveASlotSwitch`: sets
`view.ringRiskForTest() = Risk::Critical` (slot A's alarm), calls
`view.setController(slotB.controller)`, and asserts `view.getRingRisk() ==
Risk::Unavailable` immediately after — no timer tick, no sleep. This is
distinct from the existing `RingRiskHoldDoesNotSurviveASlotSwitch`, which only
proves the *hold* was cleared and never reads `ringRisk_` at all.

**Deferred nit fixed too:** `RingRiskDoesNotFlickerAcrossManyHoldWindows`
(`tests/test_spectrumview.cpp:750`) asserted `EXPECT_GT (now - 100.0, 4.0 *
kHoldMs)`. The loop runs 90 frames of `1000.0 / 30.0` ms, so `now - 100.0`
lands on exactly `3000.0` — the same value as `4.0 * kHoldMs` (750) —
an exact floating-point boundary that `EXPECT_GT` could flip on rounding.
Changed the margin to `3.9 * kHoldMs` (2925), comfortably below the ~3000 the
loop actually reaches, while still proving the run outlasted several hold
windows.

### Mutation check — RED against the pre-fix code

Reverted the one-line `ringRisk_ = RingRisk::Unavailable;` reset, rebuilt, ran
the new test alone:

```
$ cmake --build build --config Release --target HandsFreeTests
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter=*RingRiskDisplayedFieldDoesNotSurviveASlotSwitch*
[ RUN      ] SpectrumView.RingRiskDisplayedFieldDoesNotSurviveASlotSwitch
tests\test_spectrumview.cpp(831): error: Expected equality of these values:
  view.getRingRisk()
    Which is: 4-byte object <03-00 00-00>
  Risk::Unavailable
    Which is: 4-byte object <00-00 00-00>
[  FAILED  ] SpectrumView.RingRiskDisplayedFieldDoesNotSurviveASlotSwitch (40 ms)
```

`<03-00 00-00>` is `RingRisk::Critical` — slot A's alarm, still on screen
after switching to slot B. Confirms the test catches exactly the reviewer's
predicted failure. Restored the fix.

### GREEN — focused run after the fix

```
$ cmake --build build --config Release --target HandsFreeTests
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter=*RingRisk*
[----------] 11 tests from SpectrumView
[       OK ] SpectrumView.RingRiskBandsAgainstThePublishedThresholdNotAHardcodedOne (0 ms)
[       OK ] SpectrumView.RingRiskIsUnavailableWheneverTheNumberCannotBeTrusted (0 ms)
[       OK ] SpectrumView.RingRiskStepsUpImmediately (0 ms)
[       OK ] SpectrumView.RingRiskHoldsAStepDownForTheHoldTimeThenFalls (0 ms)
[       OK ] SpectrumView.RingRiskDoesNotFlickerWhileAScoreOscillatesAcrossABoundary (0 ms)
[       OK ] SpectrumView.RingRiskUnavailableOverridesTheHold (0 ms)
[       OK ] SpectrumView.RingRiskStepUpRestartsTheHold (0 ms)
[       OK ] SpectrumView.RingRiskDoesNotFlickerAcrossManyHoldWindows (0 ms)
[       OK ] SpectrumView.RingRiskFallsSevenFiftyAfterTheLastCriticalObservation (0 ms)
[       OK ] SpectrumView.RingRiskHoldDoesNotSurviveASlotSwitch (8 ms)
[       OK ] SpectrumView.RingRiskDisplayedFieldDoesNotSurviveASlotSwitch (7 ms)
[----------] 1 test from MainComponent
[       OK ] MainComponent.RingRiskReadsUnavailableUntilSomethingProvidesIt (212 ms)
[----------] 8 tests from NotchControllerRingRisk (all OK)
[  PASSED  ] 20 tests.
```

### Full suite

```
$ cd build && ctest -C Release
450/451 Test #450: SessionLogger.DestructionWithoutStopWritesSessionEnd .... Passed
451/451 Test #451: logstats_fixture ......................................... Passed

100% tests passed, 0 tests failed out of 451

Total Test time (real) =  32.26 sec
```

450 (round-1 baseline) + 1 new = 451. No new warnings for the touched files.

### Status

The re-reviewer's one open finding is closed. Concerns 1–5 of round 0 and the
notes-for-the-next-reviewer of round 1 still stand as written — in particular
no screenshot is owed until R3 wires the provider and the chip first renders
a live state.
