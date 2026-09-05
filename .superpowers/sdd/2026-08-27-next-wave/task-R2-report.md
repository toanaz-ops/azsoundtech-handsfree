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
