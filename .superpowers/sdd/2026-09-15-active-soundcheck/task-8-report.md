### Task 8 report: the five `soundcheck_*` log events

**Status:** DONE. Commit `8e85484` on `feat/lane-m-active-soundcheck`.

**Model actually used:** Opus 5 (1M context), `claude-opus-5[1m]`. The commit
trailer says `Claude Fable 5.1` because the dispatching brief required that
line verbatim (lane convention); the real model is recorded here per rule 5.

**Expected level change: 0 dB.** Log only. Nothing in this change touches a
filter coefficient, a gain, a buffer, or the ASIO callback.

## What was actually missing

Task 6 had already built four of the five events with the brief's field shapes
and the `round3sf` rounding. Verified by reading the call sites, not the report:
`src/app/SoundcheckController.cpp:255` (start), `:967` (output), `:1018`
(result), `:1112` `logAbort` (abort). `abortReasonNameForTest` and
`roundToThreeSignificantFiguresForTest` both already existed as static members
(`SoundcheckController.h:285`, `:288`), forwarding to the file-local switch and
`round3sf` (`.cpp:19`, `:46`, `:63`).

So most of this task was PROOF. One thing was genuinely missing:

**`soundcheck_apply` had no producer anywhere.** Its `placed` / `refused` /
`cleared_previous` exist only after `applySoundcheckResults` returns, on the
message thread, and invariant 17 forbids the controller holding a
`NotchController` -- so Task 6 handed the event to Task 10 (its concern C3) and
Task 10's brief calls a `logSoundcheckApply (total)` that nobody had written
(`task-10-brief.md:227`). Added:

```cpp
// SoundcheckController.h, beside SoundcheckApplyStats
[[nodiscard]] juce::var makeSoundcheckApplyEvent (const SoundcheckApplyStats& stats);
```

Task 10's `AP DUNG` lambda now has nothing to invent:
`sessionLogger_.log (makeSoundcheckApplyEvent (total));`. **No MainComponent
wiring was done here** -- `git show --stat 8e85484` lists five files and
`MainComponent.*` is not among them.

## Tests added (6, suite 662 -> 668)

`tests/test_soundcheckcontroller.cpp`, new section at `:1300`:

- `AFullRunEmitsTheFiveEventsInOrder` -- start / output x2 / result, in order,
  no abort. Uses `pumpUntil (Results, 15000)` rather than the brief's fixed
  `pump (12000)`, which is the harness's own idiom for this (`Rig::pumpUntil`,
  added in Task 6 precisely because a fixed pump spends the deadline it is
  measuring).
- `AbortEventCarriesAtOutputAndElapsed` -- `at_output` 1 (second channel),
  `elapsed_ms > 5000`, `reason == "user_stop"`, and **exactly one** abort line.
  Neither field was asserted anywhere before this; without them a tester's "it
  cut out" cannot be tied to a channel or to a moment.
- `StartCarriesTheLevelAndTheDuration` -- `outputs` 2, `peak_dbfs` -20.0,
  `sweep_ms` 3000, and `total_ms` derived from `2.0 * kPerTargetMs` rather than
  from the brief's literal 9000 +- 50. The real number is **9040**: a channel
  costs 4.52 s, not 4.5, because `kSweepLeadInMs` is real time on the clock
  (C-1). Writing 9000 into a test would have let the log and the Confirm dialog
  drift apart.
- `SoundcheckApply.TheApplyEventCarriesWhatWasPlacedAndWhatWasCleared` -- the
  new builder's shape.

`tests/test_sessionlogger.cpp` (the file-facing half):

- `SessionLoggerSoundcheck.TheFiveEventNamesAreTheirOwn` -- all five names
  distinct from each other AND from the four `ev` values logstats branches on.
- `SessionLoggerSoundcheck.RealNumbersAreRoundedToThreeSignificantFigures` --
  18.4732918273 -> 18.5, plus negative / zero / four-digit cases, called through
  the static member so the test rounds with exactly the function the writers use
  (I-8), not with a copy of its arithmetic.

**Every one of these also asserts the producer does NOT stamp `"t"`.**
`SessionLogger::log` stamps it on a one-level copy (`SessionLogger.h:65-70`); a
self-stamped `t` would be silently overwritten and would read as authoritative
in the meantime.

## The fixture, and the "no branch needed" proof

`tools/logstats.py` reads `e.get("ev")` (`:45`) and its if/elif chain has no
`else` (`:45-83`). **No branch was added.** Seven lines appended to
`tests/fixtures/session-sample.jsonl` before `session_end`, and the
`logstats_fixture` command in `tests/CMakeLists.txt:117-122` is unchanged:

```
python tools/logstats.py tests/fixtures/session-sample.jsonl
  --expect-notches 5 --expect-verdicts 3 --expect-false 1
  --expect-recurrence-max 2 --expect-retunes 2 --expect-soundcheck-replaced 1
-> exit 0, output BYTE-IDENTICAL to the run before the lines were added.
```

Seven lines, not the brief's five: the five names are all present, but an abort
with no start ahead of it reads as a corrupt log. The fixture is therefore two
runs -- one that completed and was applied (start, output, output, result,
apply) and one the operator stopped (start, abort). One `soundcheck_start`
carries a real `ring_risk` number and the other carries `null`, so both arms of
the void/number split appear in the fixture. Nothing counts fixture lines, and
none of the totals moved.

Line endings: the fixture is CRLF in the working tree and LF in the blob
(`core.autocrlf=true`, no `.gitattributes`). The append preserved the working
tree's CRLF; `git diff --numstat` reports `7 0`, i.e. added lines only. The
whole commit is `223 insertions(+)`, `0` deletions.

## Verification

```
cmake --build build --config Release          # clean
cd build && ctest -C Release
100% tests passed, 0 tests failed out of 668     (128.82 s)
```

662 -> 668, exactly the +6 predicted by the dispatching brief. (The task brief's
own Step 4 estimate of 625 was stale by three tasks.)

**Three mutations, three red** (rebuilt, run, reverted, `cmp`-verified
byte-identical revert):

| Mutation | Test killed |
|---|---|
| `makeEvent ("soundcheck_output")` -> `makeEvent ("notch_clear")` (lane G B-3 from the other direction) | `AFullRunEmitsTheFiveEventsInOrder` |
| `case AbortReason::Esc: return "user_stop";` | `EveryAbortReasonHasItsOwnName` |
| `--expect-notches 6`, and separately `--expect-verdicts 4` | `logstats_fixture`, exit 1 both times |

## Divergences from the brief -- the code won in each case

1. **`AbortReason` has NINE enumerators, not eight.**
   `NoiseFloorUnmeasured` -> `"noise_floor_unmeasured"`
   (`SoundcheckController.h:138`, `.cpp:56`). The brief's list stops at
   `capture_drop`.
2. **`EveryAbortReasonHasItsOwnName` already exists** at
   `tests/test_soundcheckcontroller.cpp:1240`, covering all nine enumerators
   with pairwise mutual separation -- strictly stronger than the brief's
   eight-name `std::set`-size check. Appending the brief's copy to
   `test_sessionlogger.cpp` would have been a weaker duplicate under a second
   suite name, so it was **not** added. The mutation check above was run against
   the existing test.
3. **`arm()` takes three arguments and returns `Refusal`.**
   `arm (targets, params, riskSnapshot)`, not `arm (targets, params)`, and not a
   `bool` (S-1: arm takes its own risk snapshot). Every brief test body's
   `ASSERT_TRUE (r.sc.arm (...))` was written as
   `ASSERT_EQ (r.armWith (r.params()), Refusal::None)` -- the harness helper that
   already defaults the snapshot.
4. **`RingRiskIsNullWhenInvalid` was already deleted** per I-7, and
   `RefusesWhenRingRiskIsRising` (`:467`) already asserts `ring_risk` is void
   when the score is not valid. Nothing to do.
5. **`tests/CMakeLists.txt` was not touched.** The brief lists it as "modify --
   no new flags beyond Task 4's"; there was literally nothing to change.
6. **`soundcheck_output` carries three fields beyond the brief's list**
   (`marked`, `ceiling_missing`, `ladder_missing`), all from Task 6 and all
   wanted by Task 9's two "no proposals, and it is not the room" branches. Left
   alone.

## Self-review

- Every event is keyed on `ev`: all five go through `SessionLogger::makeEvent`,
  whose only statement is `setProperty ("ev", name)` (`SessionLogger.cpp:25-30`).
  No `kind` key anywhere in lane M.
- Every double is rounded before it enters a `var`: `grep 'setProperty' |
  grep -v round3sf` over `SoundcheckController.cpp` leaves only ints, bools,
  strings, and the `candidates` array -- whose four real members are each rounded
  at `:989-992`.
- No `t` stamped by the controller: `grep 'setProperty ("t"'` -> no hits.
- No MainComponent wiring: not in the commit.

## Concerns

- **C1.** `makeSoundcheckApplyEvent` has no production caller until Task 10
  wires it. It is a new public header surface that nothing but a test exercises
  today -- the same shape as Task 6's C2 concern about `applySoundcheckResults`.
  If Task 10 writes its own inline event instead, this becomes dead code and
  should be deleted rather than left as a second shape for the same `ev`.
- **C2.** `soundcheck_apply` carries only the brief's three fields.
  `SoundcheckApplyStats` also holds `skippedLive`, `skippedOtherSlot` and
  `skippedBadLane` -- the last two are genuine FAULT signals (results addressed
  to the wrong slot, or naming a lane the slot does not drive) and they are now
  invisible in the log; they reach the GUI only. Worth a ruling before Task 10.
- **C3.** Nothing asserts the five events survive a REAL `SessionLogger` round
  trip to the file and back through `juce::JSON::parse`. The shape tests read the
  `var`; the fixture is hand-written text. A `t`-stamping or escaping bug between
  the two would pass both. Cheap to add in Task 10 or 11.
- **C4 (process).** The whole of `.superpowers/sdd/2026-09-15-active-soundcheck/`
  is UNTRACKED on this branch -- briefs, reports, review diffs. Lane G committed
  its ledger (`517bded`, "repo policy: .superpowers tracked"); lane M has not.
  It is outside this task's `git add` list so it was left alone, but the lane will
  finish with its whole paper trail unversioned unless someone commits it.
  (`.superpowers/sdd/.gitignore` did not exist this run; the `rm -f` was a no-op.)

---

## Fix round 1 (review of `8e85484`) -- commit `f8437df`

All three items done. Suite still green, one new mutation run.

### (1) `soundcheck_apply` now carries the two faults

`makeSoundcheckApplyEvent` (`SoundcheckController.cpp:1565`) emits
`skipped_other_slot` and `skipped_bad_lane` as plain ints beside the three
headline counts. This closes concern C2 of the original report: a result
addressed to another slot, or naming a lane the slot does not drive, reached
the GUI and nothing else -- a fault invisible to anyone reading the log
afterwards.

`skipped_live` is deliberately **NOT** logged, and both the header comment and
the implementation say why: it is a SUBSET of `refused`
(`SoundcheckController.h:418-423`), so a number sitting beside the totals
invites a reader to add it to them. The test asserts its absence, not just the
presence of the other two:

```cpp
EXPECT_TRUE (o->getProperty ("skipped_live").isVoid())
    << "skipped_live is a subset of refused and must not be logged";
```

`TheApplyEventCarriesWhatWasPlacedAndWhatWasCleared` now sets all six stats
fields to **distinct** values (3/2/4/1/5/6) so a crossed wiring cannot pass.
Mutation: swap the two sources (`skipped_other_slot` <- `skippedBadLane`,
`skipped_bad_lane` <- `skippedLive`) -> **red**, test 667. Reverted, `cmp`
byte-identical.

Fixture: the one `soundcheck_apply` line grew the two fields
(`"skipped_other_slot":0,"skipped_bad_lane":0`). `git diff --numstat` reports
`1 1` -- one line replaced, nothing else moved. Totals re-derived by running
the unchanged `logstats_fixture` command: **exit 0, every number identical**
(notches 5, retunes 2, judged 3, false 1, recurrence max 2, soundcheck
replaced 1).

### (2) `at_output` is the CURRENT output, not the last finished one

Six-line comment above the write (`SoundcheckController.cpp:1119`).
`targetIndex_` is advanced by `enterTarget()` *before* that channel emits a
sample, so a run aborted anywhere inside channel 1 -- noise floor, sweep, tail
or gap -- logs `at_output` 1 even though only channel 0 ever produced a result.
A reader who takes it for "channels measured" is off by one on **every** abort.
This is exactly what `AbortEventCarriesAtOutputAndElapsed` asserts: it stops
the run 6000 ms in, with one `soundcheck_output` logged, and expects
`at_output` 1.

### (3) Divergence missed in the first report: `soundcheck_start.gate`

The brief's field list for `soundcheck_start` is `outputs`, `peak_dbfs`,
`sweep_ms`, `total_ms`, `mode_before`, `ring_risk`. **Task 6 also emits
`gate`** (`SoundcheckController.cpp:274`) -- `params_.noiseFloorGate`, rounded
to 3 s.f., which is `getPeakinessThreshold()` frozen at Arm. It is a
PEAKINESS RATIO, never a 0..1 score (N1), and it is the ruler every
noise-floor abort in that run was judged against: without it in the line, a
`room_ringing` abort in the log cannot be told from a badly-set threshold.
Kept. It belonged in the original report's divergence list and was not there;
it is item 7 now.

Full divergence list, restated with the addition:

7. **`soundcheck_start` carries a seventh field, `gate`**, beyond the brief's
   six. Task 6's, deliberate, and load-bearing for reading any
   `room_ringing` abort. Left alone.

### Verification

```
cmake --build build --config Release
cd build && ctest -C Release -R "Soundcheck|logstats" --output-on-failure
100% tests passed, 0 tests failed out of 121

cd build && ctest -C Release
100% tests passed, 0 tests failed out of 668     (51.81 s)
```

668 unchanged -- this round added assertions to an existing test rather than a
new one. Four mutations across both rounds, four red.

### Concerns after this round

- **C2 is closed** by item (1). C1, C3 and C4 from the original report stand
  unchanged: `makeSoundcheckApplyEvent` still has no production caller until
  Task 10; nothing proves a real `SessionLogger` file round trip; and
  `.superpowers/sdd/2026-09-15-active-soundcheck/` is still untracked on this
  branch.
- **New, small.** `skipped_other_slot` and `skipped_bad_lane` are now in the
  log and in no `--expect-*` flag. `tools/logstats.py` still has no
  `soundcheck_apply` branch (correct -- it falls through), so nothing reads
  them yet. If they are meant to be a gate rather than a record, that is a
  logstats change and a separate task.
