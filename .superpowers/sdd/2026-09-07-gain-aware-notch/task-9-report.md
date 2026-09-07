# Task 9 report — the log and the preset learn about retunes

**Status:** DONE. Commit `af5201e` on `claude_desk/lane-g-brainstorm-sdd-f3c568`.
**Expected level change:** 0 dB on the audio path. Nothing in the DSP path, the
ASIO callback or buffer handling was touched — the change is the `ev` name on a
log line and what `savePreset` copies into a file.

---

## What was implemented

### B-3 closed: `notch_retune` is its own event

`src/app/MainComponent.cpp:582` — the ternary
`e.kind == Ev::Kind::Set ? "notch_set" : "notch_clear"` is now a three-way chain,
so `notch_clear` is emitted for `Kind::Clear` and nothing else. A Retune line
carries `slot`, `lane`, `index`, `hz`, `q`, `depth_db`, `origin` (the shared
head, same as a Set) plus `reason`, `from_db`, `age_ms`, and returns before the
score/`ctx` block — a retune is not a placement decision.

`retuneReasonName` is a **free** function in the anonymous namespace beside
`originName` and `reasonName` (`MainComponent.cpp:80`), as m-5 required. Four
names: `deepen` / `release` / `reclamp` / `ceiling`.

`MainComponent.h` gained `notchEventToVarForTest` in the **public** section
directly under `getNotchControllerForTest`; `notchEventToVar` itself stays a
private static. (The inline body naming a member declared later in the class is
legal — member function bodies are parsed in the complete-class context.)

### Q11: `savePreset` records what the room needed

- `pn.depthDB = sn.deepestDb` (was `sn.depthDB`). A preset saved during a quiet
  stretch now stores the deepest rung the notch ever stood on, not the rung the
  release ladder had wound back to.
- `preset.notchDefaults.Q / .depthDB` are now written from
  `notchControllers_[0]->getNotchQ() / getNotchDepthDb()`. Before this a
  reloaded preset fell back to `PresetNotchDefaults`' −12 dB and capped every
  detector notch two rungs shallower than the show was tuned at.

### `tools/logstats.py`

`notch_retune` is an **update** of the open record, never an open and never a
close: it uses `open_by_key.get(...)`, and the only `.pop(...)` in the file is
still inside the `notch_clear` branch. Each `notch_set` record gained
`depth_db` (running), `deepest_db` (min seen) and `retunes` (count). The table
grew a `depth` / `deep` / `rt` triple, the summary line grew `retunes N`, and
`--expect-retunes` was added next to the other `--expect-*` flags. `load()`'s
`encoding="utf-8"` was left untouched (repo rule 6); the tool still writes
nothing back.

The `if/elif` chain with no `else` was preserved deliberately: an old reader
meeting a newer log ignores the unknown event rather than miscounting.

### The fixture

`tests/fixtures/session-sample.jsonl` now tells one coherent story for the
lane-1 index-0 notch, per m-7 / m-A:

| t (ms) | event | depth | note |
|---|---|---|---|
| 5001.0 | `notch_set` | −6 | was −18; a lane-G placement is the shallowest rung |
| 5301.0 | `notch_retune` deepen | −6 → −12 | exactly `kDeepenAfterMs` = 300 later |
| 35400.0 | `notch_retune` release | −12 → −6 | ≥ `kReleaseFirstMs` = 30 000 after the last depth change (5301 + 30 000 = 35 301; 99 ms of slack). `age_ms` 30 399 = 35 400 − 5001, measured from the SET |
| 45400.0 | `notch_clear` auto_release | — | exactly `kReleaseStepMs` = 10 000 after the release; `age_ms` 40 399 |

`session_end` stays at 60 000; the file is still in ascending `t` order.
Constants checked against `src/app/NotchController.h:118,125,126` in this
worktree: `kDeepenAfterMs 300.0`, `kReleaseFirstMs = kAutoReleaseMs 30000.0`,
`kReleaseStepMs 10000.0`.

`tests/CMakeLists.txt` `logstats_fixture` gained `--expect-retunes 2`; the other
four expectations are unchanged and still pass, exactly as the brief predicted.

---

## Tests

Seven new tests (526 → 535 ctest entries; 528 → 535 because gtest_discover_tests
lists each GoogleTest case).

`tests/test_gui_wiring.cpp`
1. `SavePresetRecordsTheDeepestDepthNotTheRestingOne` — place −18 Detector,
   `retuneForTest(Release, −6)`, publish a snapshot, save, reload: depth is −18.
2. `SavePresetRoundTripsTheCeilingThroughNotchDefaults` — `setNotchDefaults(44,
   −24)`, save, reload: `notchDefaults.Q == 44`, `.depthDB == −24`.
3. `RetuneEventsAreLoggedUnderTheirOwnEventName` — the serialiser directly: `ev`,
   `reason`, `origin`, `from_db`, `depth_db`, `age_ms`, `slot`, `lane`, `index`,
   `hz`, `q`, and **no** `score` / `ctx`.
4. `SetAndClearKeepTheirOwnEventNames` — added beyond the brief: proves
   `notch_set` / `notch_clear` did not pick up the new name and that a Clear
   carries no `from_db`. This is the other half of "`notch_clear` only for
   `Kind::Clear`".
5. `EveryRetuneReasonHasItsOwnName` — all four strings.
6. `ARetuneOnALiveNotchIsWrittenToTheSessionLog` — added beyond the brief and the
   one that proves the wiring rather than the function: a real `MainComponent`
   with `startSessionLog`, a real `setNotch` + `retuneForTest(Deepen)`, a real
   `runOnce()` flush, then the written file is parsed. Asserts a `notch_retune`
   line exists with the right fields, that it comes after the `notch_set`, and
   that **no `notch_clear` was written** — the record stayed open.

`tests/test_presetmanager.cpp`
7. `TheFullLadderRangeSurvivesTheRoundTrip` — −24 dB (the ladder's deepest rung)
   survives as both a `notchDefaults` depth and a notch depth.

### TDD evidence

**RED, `logstats.py`** (before Step 5):

```
logstats.py: error: unrecognized arguments: --expect-retunes 2
```

**RED, the suite** (tests written, implementation not yet):

```
[  FAILED  ] 6 tests, listed below:
[  FAILED  ] GuiWiring.SavePresetRecordsTheDeepestDepthNotTheRestingOne
[  FAILED  ] GuiWiring.SavePresetRoundTripsTheCeilingThroughNotchDefaults
[  FAILED  ] GuiWiring.RetuneEventsAreLoggedUnderTheirOwnEventName
[  FAILED  ] GuiWiring.EveryRetuneReasonHasItsOwnName
[  FAILED  ] GuiWiring.ARetuneOnALiveNotchIsWrittenToTheSessionLog
[  FAILED  ] PresetManager.TheFullLadderRangeSurvivesTheRoundTrip
```

with, for the log test, `error: Expected: (retune) != (nullptr), actual: NULL —
no notch_retune line in the session log`. `SetAndClearKeepTheirOwnEventNames`
passed at RED, correctly: Set and Clear already worked.

### GREEN — the log tool by hand

```
$ python tools/logstats.py tests/fixtures/session-sample.jsonl \
    --expect-notches 4 --expect-verdicts 3 --expect-false 1 \
    --expect-recurrence-max 2 --expect-retunes 2
session   1m00s  app 1.1.2  os Windows 11
device    'Fake ASIO'  48000 Hz  buffer 256
modes     0.0s:auto

  # slot lane       hz origin       depth   deep  rt     held verdict  cleared by
  1    0    R   1007.8 detector      -6dB  -12dB   2    40.4s -        auto_release
  2    0    L   2437.5 detector     -18dB  -18dB   0     2.0s false    verdict_false
  3    0    R   1007.8 detector     -18dB  -18dB   0    40.0s good     still active
  4    0    L    482.0 preset       -12dB  -12dB   0    30.0s good     still active

recurrence (Hz within one bin, count of placements):
    1007.8 Hz  x2

notches 4  retunes 2  judged 3  false 1  false-rate 33%  unjudged 1 (25%)
EXIT=0
```

Row 1 is the whole point: `-6dB` running, `-12dB` deepest, 2 retunes, held 40.4 s
(5001 → 45400) and cleared by `auto_release`. Before this change the same notch
read `held 36.0s` with no depth column at all, and the two retune lines fell
through every branch unseen.

### GREEN — focused

```
$ cmake -B build -G "Visual Studio 18 2026" -A x64      # header + CMakeLists changed
$ cmake --build build --config Release
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter='GuiWiring.*:PresetManager.*'
[==========] 75 tests from 2 test suites ran. (10337 ms total)
[  PASSED  ] 75 tests.
```

### GREEN — full gate

```
$ cd build && ctest -C Release
535/535 Test #535: logstats_fixture ......................  Passed  0.12 sec

100% tests passed, 0 tests failed out of 535

Total Test time (real) =  53.70 sec
```

---

## Deviations from the brief

1. **`PresetManager.TheFullLadderRangeSurvivesTheRoundTrip` could not use the
   brief's literal body.** A bare `Preset p;` fails validation before it reaches
   the depth the test is about: `"bufferSize" must be greater than 0, but is 0`
   (observed at RED). Rewritten on top of the file's own `makeValidPreset()`
   helper, which supplies `version`, `device`, `sampleRate` and `bufferSize`;
   the assertions are the brief's, unchanged.
2. **Two tests added beyond the brief's four.**
   `SetAndClearKeepTheirOwnEventNames` pins the negative half of B-3, and
   `ARetuneOnALiveNotchIsWrittenToTheSessionLog` proves the retune reaches a real
   file through the real wiring — the brief's serialiser tests would all still
   pass if the event never got logged at all. The parent task explicitly asked
   for the "session log written by the running app" test.
3. **The build did not fail at Step 3 the way the brief predicted.** The header
   wrapper was in place before the first build, so the compile error the brief
   expected never occurred; RED was six failing tests at runtime instead, which
   is the stronger evidence anyway.
4. **`docs/superpowers/specs/2026-09-05-data-loop-design.md` was NOT touched.**
   It is not in this brief's file list (brief §Files names only
   `MainComponent.cpp/.h`, `logstats.py`, the fixture, `tests/CMakeLists.txt` and
   the two test files), and the brief's own Interfaces section says "Task 10
   documents it". **The schema line belongs to Task 10.**
5. **A `--expect-retunes` mention was added to `logstats.py`'s module docstring
   usage line** — one line, keeping the docstring's example in step with the
   flags it documents.

---

## Files changed

- `src/app/MainComponent.cpp` — `retuneReasonName` (free, anon namespace); the
  three-way `evName`; the Retune branch in `notchEventToVar`; `pn.depthDB =
  sn.deepestDb`; `preset.notchDefaults` from the controller.
- `src/app/MainComponent.h` — public `notchEventToVarForTest` wrapper.
- `tools/logstats.py` — the `notch_retune` update branch, the three new record
  fields, the widened table, the `retunes` total, `--expect-retunes`.
- `tests/fixtures/session-sample.jsonl` — 13 → 15 lines; the lane-1 index-0
  notch's whole life made ladder-consistent.
- `tests/CMakeLists.txt` — `--expect-retunes 2` on `logstats_fixture`.
- `tests/test_gui_wiring.cpp` — six new tests.
- `tests/test_presetmanager.cpp` — one new test.

Nothing under `.superpowers/` is staged; `.superpowers/sdd/.gitignore` was
removed before the commit (the SDD-workspace trap in
`memory/sdd-workspace-gitignore-trap-2026-09-04.md`). `git diff --cached
--name-only` showed exactly the seven files above.

`NotchController`, `NotchCommand`, `AudioEngine` and `SessionLogger.cpp` were not
modified.

---

## Self-review

- **`notch_clear` only for `Kind::Clear`.** `grep -rn "notch_clear\|notch_set\|
  notch_retune" src/` returns exactly one emission site — the `evName` chain at
  `MainComponent.cpp:582-584`. `Kind` has three enumerators, so the chain's final
  branch is reachable only for `Clear`. Test 4 pins it from the other side.
- **logstats never pops on `notch_retune`.** `grep -n "pop\|open_by_key"
  tools/logstats.py` → `.pop(` appears once, on line 79, inside the
  `elif ev == "notch_clear"` branch. The retune branch (line 67) and the verdict
  branch (line 75) both use `.get(`.
- **The fixture obeys the ladder.** Verified numerically at write time (the
  script asserts ascending `t` and re-parses every line as JSON) and against the
  real constants: release at 35 400 ≥ 5301 + 30 000; clear at 45 400 =
  35 400 + 10 000 exactly. `age_ms` on both later lines is measured from the SET
  at 5001, matching `pushRetuneLocked`'s `ev.ageMs = liveMs_ - n.lockedAtMs`.
- **The two savePreset tests are not accidentally passing.** In test 1 the
  ceiling clamp at `NotchController.cpp:782` (`n.deepestDb = std::max(n.deepestDb,
  ceilingDbFor(n))`) is a no-op because the default ceiling is exactly
  `kDefaultNotchDepthDb = -18.0`; `depthDB` −6 is not deeper than −18, so no
  `Ceiling` retune fires either. The −18 that comes back is the placement's
  `deepestDb`, not an artefact of the clamp.
- **No line-ending churn.** `git diff --cached --stat` is 297 insertions /
  10 deletions across the seven files — no whole-file rewrite.

## Concerns / notes for the next task

1. **Task 10 owns the schema doc.** `docs/superpowers/specs/2026-09-05-data-loop-
   design.md` still describes only `notch_set` / `notch_clear`. The producing
   side now emits `notch_retune`; until Task 10 lands, the design doc and the
   code disagree.
2. **The tester-facing docs are untouched by design.** Per the project's
   definition-of-done item 5, a change to user-visible behaviour also updates
   `docs/GIOI-THIEU.md` and `docs/KY-THUAT-CHONG-HU.md`. This task's file list
   forbids touching them, and the visible behaviour it changes (a preset now
   saves the deepest depth and the ceiling) is worth a line in both. **Flagging
   for whoever closes lane G**, not something to leave to the release note alone.
3. **Old preset files are unaffected and old logs still read.** A pre-lane-G
   preset has no `notchDefaults` written by this app but the loader already
   defaults it; a pre-lane-G log has no `notch_retune` lines and reports
   `retunes 0`. Neither direction breaks.
4. **`getNotchQ()/getNotchDepthDb()` are read from slot 0 only.** That matches
   how `notchDefaults` is a single global pair in the preset format, and how the
   TuningPanel drives every slot from one control — but if per-slot ceilings ever
   arrive, this line becomes lossy. Worth a comment in the format spec.

---

## Fix round 1

**Status:** all three findings addressed. Full suite `539/539` (was 535 — four new
tests). **Expected level change: 0 dB.** Nothing in the DSP path, the ASIO
callback or buffer handling was touched; the one behavioural change is that a
load can now move the *ceiling*, which caps FUTURE deepening only — the notches
the file installs are still adopted at their own file depth by `adoptPreset`.

### Important 1 — the ceiling is written on save but never applied on load

The write side existed since `af5201e`; nothing read it back. Implemented the
read side in three pieces.

**1. `Preset` can now say whether the FILE carried the block.**
`src/app/PresetManager.h:162-174` — new `bool hasNotchDefaults = false;` with the
reason it exists in the doc comment. `src/app/PresetManager.cpp:716-741` (the flag itself at `:735`) — set
`true` inside `fromJSON`'s existing optional-`notchDefaults` branch, on the arm
that actually reads numbers out of an object. A malformed `"notchDefaults"` (not
an object) still errors and leaves the flag false, so a file that never parsed
cannot be reported as an operator's chosen ceiling. `saveToFile`/`toJSON` are
unchanged and still always emit the block.

This is the M-3 trap: without the flag, "absent" and "present and −12" are the
same value, and applying the fallback would drag a rig tuned at −18 back two
rungs every time a v1 preset was opened.

**2. `loadPreset` applies it.** `src/app/MainComponent.cpp:924-963`, placed after
the `adoptPreset` loop and after the detectors are restarted (`setNotchDefaults`
writes two atomics; it does not need a stopped thread).

**The routing decision — global vs per-slot.** Mirrors the `TuningPanel`
handler at `MainComponent.cpp:480-495`, which is the only other place a *global*
tuning change fans out: loop `0..kMaxSlots`, `continue` on any slot whose
`slotUsesGlobalTuning_[i]` is false, `setNotchDefaults` on the rest. Rationale:

- the preset format carries **one** global pair (`PresetNotchDefaults`), and
  `savePreset` reads it from slot 0 — there is nothing per-slot in the file to
  restore, so applying it to a Custom slot would invent tuning the file never
  recorded;
- a slot on Custom keeps its own values until its Tune combo goes back to G,
  which is exactly what the panel does for a global change. A load behaving
  differently from the strip would be a second, invisible rule;
- default is `slotUsesGlobalTuning_.fill(true)` (`:180`), so on a normal rig
  every slot follows — asserted in the new test via slot 1.

`setNotchDefaults` clamps to Q 8..50 / −24..−6 dB, so a hand-edited file cannot
push the controller outside the range the panels can reach. No validation was
removed anywhere.

**GUI refresh:** `tuningPanel_.refresh()` is called in the same `if`. The strip
is provider-driven (`src/gui/TuningPanel.h:61,68`; `paramsProvider` reads
`notchControllers_[0]` at `MainComponent.cpp:507-514`), so `refresh()` re-reads
the controller and both combos show what the file installed.
`slotPanel_.refresh()` was already called at the end of `loadPreset` and covers
the per-slot editor.

**3. The comment.** `src/app/MainComponent.cpp:1066-1076` (`savePreset`) no longer
claims the round-trip happens by itself; it now names `loadPreset` as the reader
and says which slots it reaches, plus why slot 0 is the source.

**Tests.**

`tests/test_presetmanager.cpp:532-563` — `AnAbsentNotchDefaultsBlockIsReportedAsAbsent`
(`kValidPresetJson`, which has no block: flag false, fallback values still filled)
and `APresentNotchDefaultsBlockIsReportedAsPresent` (round-trip carrying the *same*
−12/30 the fallback supplies, so the test pins provenance rather than a value
difference).

`tests/test_gui_wiring.cpp:1243-1319` — `LoadPresetAppliesTheCeilingTheFileCarries`
(tune Q 30 / −18, save, drag the controls to Q 8 / −6, load: controller 0 is back
at −18/30, and slot 1 follows) and `LoadPresetLeavesTheCeilingAloneWhenTheFileCarriesNone`
(a hand-written v1 JSON with no `notchDefaults` — `savePreset` always writes one
now, so it cannot be produced by the app — leaves a live −18/30 untouched).

**RED** (`hasNotchDefaults` present as an inert field, nothing setting or reading
it):

```
$ ./build/tests/Release/HandsFreeTests.exe \
    --gtest_filter='GuiWiring.LoadPreset*:PresetManager.*NotchDefaultsBlock*'
tests/test_gui_wiring.cpp: error: EXPECT_DOUBLE_EQ(controller0->getNotchDepthDb(), -18.0)
  Which is: -6 ... 30.0 Which is: 30   [Q was 8]
tests/test_presetmanager.cpp(561): error: Value of: result.preset.hasNotchDefaults
  Actual: false
Expected: true

[==========] 6 tests from 2 test suites ran. (3714 ms total)
[  PASSED  ] 4 tests.
[  FAILED  ] 2 tests, listed below:
[  FAILED  ] GuiWiring.LoadPresetAppliesTheCeilingTheFileCarries
[  FAILED  ] PresetManager.APresentNotchDefaultsBlockIsReportedAsPresent
```

`LoadPresetLeavesTheCeilingAloneWhenTheFileCarriesNone` passed at RED, correctly —
nothing was applied at all then. It is the guard that the fix does not overshoot.

### Important 2 — `retuneReasonName` fallthrough

`src/app/MainComponent.cpp:93-96`: `return "deepen"` became `return "unknown"`,
matching `originName` (`:63`) and `reasonName` (`:77`). An enumerator added
without a name here now reads as `unknown` in the log instead of masquerading as
a deepening that never happened. `EveryRetuneReasonHasItsOwnName` needed **no**
adjustment — it asserts only the four named reasons and all four are unchanged.

### Minor 3 — `tools/logstats.py` retune branch

`tools/logstats.py:59-79`: `depth_db` is now guarded exactly like `deepest_db`
was. A retune line without `depth_db` no longer erases the running depth the
`notch_set` established (and no longer knocks `deepest_db` to `None` through the
`is None` arm); the retune still counts, since it happened. Checked by hand
against a copy of the fixture with `"depth_db":-12` stripped from the deepen line:

```
  # slot lane       hz origin       depth   deep  rt     held verdict  cleared by
  1    0    R   1007.8 detector      -6dB   -6dB   2    40.4s -        auto_release
EXIT=0
```

The record stays open, keeps a real depth and still reports 2 retunes.

### Commands and output

```
$ cmake --build build --config Release
BUILD EXIT=0   (only the pre-existing juce::Displays::userArea C4996 warning)

$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter='GuiWiring*:PresetManager*'
[==========] 79 tests from 2 test suites ran. (10415 ms total)
[  PASSED  ] 79 tests.

$ cd build && ctest -C Release -R logstats --output-on-failure
1/1 Test #539: logstats_fixture .................   Passed    0.12 sec
100% tests passed, 0 tests failed out of 1

$ cd build && ctest -C Release
539/539 Test #539: logstats_fixture ...   Passed    0.12 sec
100% tests passed, 0 tests failed out of 539
Total Test time (real) =  46.87 sec
```

### Files touched

`src/app/MainComponent.cpp`, `src/app/PresetManager.h`, `src/app/PresetManager.cpp`,
`tools/logstats.py`, `tests/test_gui_wiring.cpp`, `tests/test_presetmanager.cpp`.
`MainComponent.h` needed no change. `NotchController`, `AudioEngine` and `docs/`
were not touched.

### Concerns still open

1. **Concern 4 of the original report is now load-bearing, not hypothetical.**
   The ceiling is saved from slot 0 and restored to every Global slot. If
   per-slot ceilings ever ship, both ends become lossy together — the format
   needs a per-slot block before that lands.
2. **A Custom slot silently ignores a preset's ceiling.** That is the panel's own
   rule and is deliberate, but nothing tells the operator: no toast, no
   `preset_load` field. Worth one key on the `preset_load` event if lane G's
   closer wants it.
3. **Docs unchanged by instruction.** `docs/GIOI-THIEU.md` and
   `docs/KY-THUAT-CHONG-HU.md` still do not mention that a preset now carries and
   restores the ceiling — item 5 of the project's definition of done, still owed
   by whoever closes lane G (as flagged in the original report).

---

## Fix round 2

**Status:** both findings and the out-of-scope observation addressed. Full suite
`546/546` (was 539 — seven new tests). Nothing committed; the files are left in
the working tree.

### THE CORRECT EXPECTED LEVEL STATEMENT

The round-1 line "0 dB, future-only" was **false** and is retracted.

Loading a preset whose ceiling is SHALLOWER than a live Detector notch raises
that notch to the ceiling on the next detector tick — **up to +14 dB at that
bin** (Music.json's −10 dB over a −24 dB notch), as one ramped step of 10 ms.
That is a bin which was, by construction, the one ringing.

The mechanism, read out of the code rather than out of the round-1 note:
`NotchController::ceilingDbFor` (`src/app/NotchController.cpp:173-177`) returns
the LIVE `notchDepthDb_` for any notch whose `ModelNotch::ceilingDb` is NaN —
which is exactly the Detector-origin notches. The detection pass then hits
`if (n.depthDB < ceiling)` (`NotchController.cpp:785-796`) and calls
`pushRetuneLocked (…, RetuneReason::Ceiling)`; its own comment says the step is
"possibly more than 6 dB".

- **Shallower ceiling loaded:** every live Detector notch deeper than it comes
  up to it, next tick, ramped 10 ms. Worst case among the shipped presets is
  Music.json's −10 dB over a −24 dB notch = **+14 dB**.
- **Deeper ceiling loaded:** 0 dB now; only deeper future rungs are unlocked.
- **Preset / Manual / Soundcheck notches:** untouched either way — they carry
  their own `ceilingDb` (Q8), so `ceilingDbFor` never reads the global value for
  them. So are the notches this load itself just adopted, at their file depth.
- **This round changes none of that.** The ceiling pull is Q1/Q8 by design and
  no DSP file was touched. What changed is that the load now SAYS it moved the
  ceiling. The tester notes still have to carry the behaviour (controller's job).

### New 2 (Important) — an off-list ceiling blanked the strip, and the next touch reset it

Confirmed at RED in the failure text itself: after a provider hands Q 25 /
−10 dB, a change on RISE reported `depthDb` **−6** and `q` **10**.

`idForValue` returns 0 for a value that is not one of the fixed rungs
(`TuningPanel.cpp:23-29`); `refresh()` fed that 0 to `setSelectedId`, which
empties the combo; `currentParams()` then read `qForId(0)` = 10 and
`depthDbForId(0)` = −6 through `valueForId`'s `return choices[0]`. Every combo
is wired to `handleChanged()` and every callback carries the COMPLETE snapshot,
so one touch on RISE pushed Q 10 / −6 dB to every Global slot. Reachable for the
first time in round 1, when `loadPreset` began installing a file's ceiling.

Fixed in both panels, for all five parameters — not only depth and Q: HOLD, RISE
and THR have the same shape, and the uniform version is the smaller change.

**TuningPanel**

- `src/gui/TuningPanel.cpp:40-59` — new `selectOrShow` free template beside
  `idForValue`: select the item, or `setText` the real value. `ComboBox::setText`
  matches an existing item by text first, so an on-list value can never take the
  text-only path.
- `src/gui/TuningPanel.cpp:233-255` (`refresh()`) — `selectOrShow` for rise /
  depth / Q / thr, in the same formats the ctor's `addItem` calls use
  (`"-10 dB"`, `"25"`, `"250 ms"`, one decimal for THR); HOLD written out
  because its item id IS its value. Stores the snapshot in `provided_`.
- `src/gui/TuningPanel.cpp:274-296` (`currentParams()`) — starts from
  `provided_` and overwrites only a field whose combo has a real selection.
- `src/gui/TuningPanel.cpp:209-210` (`applyPreset`) — a curated preset is
  entirely on-list, so it also becomes `provided_`; keeps it from going stale.
- `src/gui/TuningPanel.h:139-146` — the `provided_` member, with the reason.

**SlotPanel** — shares the rung lists AND `TuningPanel::idForX`, so its per-slot
editor had the identical bug.

- `src/gui/SlotPanel.cpp:64-76` — an `int`-id `selectOrShow` in the anon
  namespace.
- `src/gui/SlotPanel.cpp:380-411` (`seedDetailFrom`) — the same five, plus
  `seeded_`.
- `src/gui/SlotPanel.cpp:414-441` (`currentTuning()`) — starts from `seeded_`,
  keeps `usesGlobal = false`, and overwrites only a selected combo.
- `src/gui/SlotPanel.h:201-209` — the `seeded_` member.

**Tests (5).** `tests/test_tuningpanel.cpp:193` — the labels show `-10 dB` and
`25` and the three on-list combos still select normally; `:221` — a RISE touch
re-emits Q 25 / −10; `:252` — picking `-12 dB` emits −12 while Q stays 25.
`tests/test_slotpanel.cpp:366` — the detail editor seeds as text AND the first
report `openDetailFor` sends already carries −10 / 25; `:392` — a RISE edit keeps
both, then picking `-12 dB` applies −12 and Q stays 25.

**RED** (all five failed before the fix):

```
tests/test_slotpanel.cpp(382): error: panel.getDetailForTest (0).depth.getText()
    Which is:                          <-- empty
  juce::String ("-10 dB")
tests/test_slotpanel.cpp(388): error: reported.depthDb
    Which is: -6        -10.0
tests/test_slotpanel.cpp(389): error: reported.q
    Which is: 10        25.0
tests/test_tuningpanel.cpp(248): error: reported.depthDb
    Which is: -6        -10
tests/test_tuningpanel.cpp(249): error: reported.q
    Which is: 10        25
```

### New 1 (Important) — the load now logs the ceiling it applied

`src/app/MainComponent.cpp:941-1000` — the comment claiming "Expected level
change: 0 dB right here" is replaced by the statement above, naming the file and
the branch that does the pull. The apply loop now reads the ceiling **back** off
the first Global controller (`:977-986`) instead of echoing the file's numbers,
so the log carries what is actually standing after `setNotchDefaults`' Q 8..50 /
−24..−6 clamp — and with every slot on Custom the loop never runs and
`ceilingApplied` stays false, which is the truth: nothing was applied.

`src/app/MainComponent.cpp:1010-1028` — the `preset_load` event gained
`ceiling_applied` (always written, so a reader can tell "this load moved
nothing" from "the field is new") plus `q` and `depth_db` as doubles, written
only when one was applied. Same names and same type as the `tuning` event, so
logstats would read one column for both.

**Tests (2).** `tests/test_gui_wiring.cpp:1699`
`PresetLoadLogsTheCeilingItApplied` — save a show tuned Q 30 / −18, load it under
a session log, assert `ceiling_applied` true and `q` 30 / `depth_db` −18.
`:1740` `PresetLoadWithoutACeilingSaysSoInTheLog` — a hand-written v1 file with
no `notchDefaults`: `ceiling_applied` present and false, and **no** `q` /
`depth_db` keys.

**RED:**

```
tests/test_gui_wiring.cpp(1732): error: Value of: (bool) (*load)["ceiling_applied"]
  Actual: false   Expected: true
tests/test_gui_wiring.cpp(1733): error: (double) (*load)["q"] evaluates to 0, 30.0 evaluates to 30
tests/test_gui_wiring.cpp(1734): error: (double) (*load)["depth_db"] evaluates to 0, -18.0 evaluates to -18
tests/test_gui_wiring.cpp(1765): error: load->getDynamicObject()->hasProperty ("ceiling_applied")
  Actual: false   Expected: true
```

### Out-of-scope observation

`src/app/MainComponent.cpp:1128-1130` — the `savePreset` comment cited "the
task-9 report", an SDD workspace file that does not survive the workspace. It
now cites `docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md` §4.8 /
Q11; both anchors were checked to exist in that file (`:329`, `:351`).

### Commands and output

RED, the seven new tests before any implementation:

```
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter='TuningPanel.*OffList*:TuningPanel.*Rung*:SlotPanel.*OffList*:SlotPanel.EditingOneDetailCombo*:GuiWiring.PresetLoad*Ceiling*'
[==========] 7 tests from 3 test suites ran. (413 ms total)
[  PASSED  ] 0 tests.
[  FAILED  ] 7 tests, listed below:
[  FAILED  ] SlotPanel.AnOffListSlotTuningIsSeededAsTextRatherThanBlankingTheCombos
[  FAILED  ] SlotPanel.EditingOneDetailComboKeepsTheOffListOnesUnchanged
[  FAILED  ] TuningPanel.AnOffListCeilingIsShownAsTextRatherThanBlankingTheCombo
[  FAILED  ] TuningPanel.TouchingAnotherComboReSendsTheOffListCeilingUnchanged
[  FAILED  ] TuningPanel.PickingARungFromAnOffListCeilingAppliesThatRung
[  FAILED  ] GuiWiring.PresetLoadLogsTheCeilingItApplied
[  FAILED  ] GuiWiring.PresetLoadWithoutACeilingSaysSoInTheLog
```

GREEN:

```
$ cmake -B build -G "Visual Studio 18 2026" -A x64     # two headers changed
$ cmake --build build --config Release
BUILD EXIT=0   (only the pre-existing juce::Displays::userArea C4996 warning)

$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter='TuningPanel*:SlotPanel*:GuiWiring*'
[==========] 37 tests from 3 test suites ran. (10314 ms total)
[  PASSED  ] 37 tests.

$ cd build && ctest -C Release
546/546 Test #546: logstats_fixture ...   Passed    0.11 sec
100% tests passed, 0 tests failed out of 546
Total Test time (real) =  43.03 sec
```

### The pictures

```
$ build/tools/Release/HandsFreeSnapshot.exe shots-r2 1440 920 --fast
wrote ...\shots-r2\console-idle.png (1440 x 920)
console-live: RING RISK chip STAGED to Critical
wrote ...\shots-r2\console-live.png (1440 x 920)
```

`shots-r2/console-live.png`, read back: nothing visual regressed on an on-list
value. DETECTION still reads `NOTCH DEPTH [-18 dB]  Q [30]` and `TRIGGER RISE
[250 ms]  HOLD [3]  THR [10.0]`, RESPONSE is lit on Balanced with no CUSTOM
chip, and the trace, markers, notch list and routing table are unchanged.

`shots-r2/console-offlist-ceiling.png` — the off-list case. `HandsFreeSnapshot`
has no flag to stage a ceiling and `tools/snapshot.cpp` is not in this round's
file list, so it was rendered from a **temporary** test in
`tests/test_gui_wiring.cpp` mirroring the tool (same channel-name injection,
same 1440x920, same `createComponentSnapshot`) with all eight controllers set to
Q 25 / −10 dB. Read back: `NOTCH DEPTH [-10 dB]  Q [25]`, both legible in the
real condensed face, right-aligned and carrying their carets exactly like an
on-list value, no blank field anywhere; the CUSTOM chip is lit, correctly,
because −10 / 25 matches none of the three curated sets. **The temporary test
was then removed** — the file was restored from a byte copy taken before it was
added, `grep -c TempRenderOffListCeiling tests/test_gui_wiring.cpp` is 0, and
the 546-test ctest run quoted above is the post-removal one.

### Files touched

- `src/gui/TuningPanel.h`, `src/gui/TuningPanel.cpp`
- `src/gui/SlotPanel.h`, `src/gui/SlotPanel.cpp`
- `src/app/MainComponent.cpp`
- `tests/test_tuningpanel.cpp`, `tests/test_slotpanel.cpp`,
  `tests/test_gui_wiring.cpp`

`git diff --stat`: 448 insertions / 34 deletions across those eight — no
whole-file rewrite and no line-ending churn (every line of all eight is CRLF).
No `NotchController`, `AudioEngine`, `PresetManager`, `tools/` or `docs/`
change. Nothing was staged or committed; there is no `.superpowers/sdd/.gitignore`
in this worktree.

### Concerns

1. **`shots-r2/` is untracked and NOT gitignored** — `.gitignore:73` covers
   `shots/` only. It must be kept out of the commit.
2. **The tester notes still owe the level statement.** The ceiling pull is real
   and user-visible; per definition-of-done item 5 it belongs in
   `docs/GIOI-THIEU.md` and `docs/KY-THUAT-CHONG-HU.md`, still outside this
   round's file list. Third round flagging it.
3. **`ceiling_applied` is in no schema doc**, and `tools/logstats.py` does not
   read the new keys — both files are out of this round's list. Nothing breaks:
   `preset_load` is not an event logstats parses today.
4. **A mixed rig still says nothing about the slots that did not follow.**
   `ceiling_applied` false now covers the all-Custom case, but with some slots
   Global and some Custom the event logs `true` and the Custom ones silently
   keep their own ceiling (round-1 concern 2, narrowed rather than closed).
