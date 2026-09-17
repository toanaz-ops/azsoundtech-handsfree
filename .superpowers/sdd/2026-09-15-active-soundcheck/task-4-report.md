# Task 4 report — the two additive `NotchController` changes, the logstats tally, the fixture

**Status:** DONE. Commit `feebf82e22e5bcc6438f651c3e7d8133e21abb22` on `feat/lane-m-active-soundcheck`
(parent `23c99f1`). **585/585 `ctest -C Release`**, 0 failed.

---

## 1. What was done

Five edits, all additive. Nothing that existed before changes behaviour: `setNotch`,
`clearNotch`, the release ladder, and every existing `ClearReason` path are untouched.

| File | Change |
|---|---|
| `src/app/NotchController.h:69` | `ClearReason` gains `SoundcheckReplace` — appended LAST, so no existing enumerator's value moves. |
| `src/app/NotchController.h:403` | `SnapshotNotch` gains `Origin origin = Origin::Detector;`, after `deepestDb`, BEFORE `channel`/`index`, exactly as the brief specifies. |
| `src/app/NotchController.cpp:580` | The ONE aggregate initialiser gains `n.origin,` on its own line, in the same edit. |
| `src/app/MainComponent.cpp:76` | `reasonName` gains `case ...SoundcheckReplace: return "soundcheck_replace";` |
| `tools/logstats.py` | `soundcheck_replaced` tally in `summarise()`, a summary line in `print_report()`, `--expect-soundcheck-replaced` + its check in `main()`. |
| `tests/fixtures/session-sample.jsonl` | One `notch_set` (`origin: soundcheck`) + its `notch_clear` (`reason: soundcheck_replace`), inserted BEFORE `session_end`. |
| `tests/CMakeLists.txt:116-118` | `--expect-notches 4 → 5`, `--expect-soundcheck-replaced 1` added. |
| `tests/test_notchcontroller.cpp` (+87) | `NotchControllerSoundcheck.SnapshotCarriesOrigin`, `...SoundcheckReplaceIsItsOwnClearReason`. |
| `tests/test_gui_wiring.cpp` (+30) | `GuiWiring.SoundcheckReplaceReachesTheLogAsItsOwnReason`. |

`git show --stat feebf82` — exactly the eight files the brief lists, nothing else:

```
 src/app/MainComponent.cpp           |  1 +
 src/app/NotchController.cpp         |  1 +
 src/app/NotchController.h           | 14 +++++-
 tests/CMakeLists.txt                |  5 ++-
 tests/fixtures/session-sample.jsonl |  2 +
 tests/test_gui_wiring.cpp           | 30 +++++++++++++
 tests/test_notchcontroller.cpp      | 87 +++++++++++++++++++++++++++++++++++++
 tools/logstats.py                   | 13 ++++++
 8 files changed, 150 insertions(+), 3 deletions(-)
```

The whole `src/` side of the diff is three functional lines:

```
+        case NotchController::ClearReason::SoundcheckReplace:  return "soundcheck_replace";
+                                                n.origin,
-        Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind
+        Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind,
+        SoundcheckReplace
+        Origin origin = Origin::Detector;
```
(plus the two comment blocks the brief dictates).

---

## 2. Anchors — all verified against the real code before editing

Every line anchor in the brief was correct at `23c99f1`. Verified by anchor TEXT, not line number:

- `ClearReason` at `NotchController.h:67-70`, list on `:69`. OK
- `struct SnapshotNotch` at `NotchController.h:394-406`, `std::uint8_t index   = 0;` on `:405`. OK
- `notchList[notchCount++] = { (float) n.frequency, (float) n.Q,` at `NotchController.cpp:578-580`.
  OK, and it is the ONE site: `grep -rn SnapshotNotch src/ tests/ tools/` finds no other aggregate
  initialiser, and no `sizeof`/`memcpy` that assumes the layout.
- `case ...PartialApplyUnwind: return "partial_apply_unwind";` at `MainComponent.cpp:75`. OK
  (m-2's correction from `:74` holds).
- `Recorder` at `test_notchcontroller.cpp:1471` inside the anon namespace `:1463-1487`. OK — its
  own comment carries both B-2 traps verbatim.
- `SetAndClearKeepTheirOwnEventNames` at `test_gui_wiring.cpp:1356-1369`. OK
- `logstats.py` anchors at `:19` (utf-8), `:46-87` (if/elif, no else), `:111`, `:145`, `:157`,
  `:174`. OK
- fixture line 15 is `session_end` (m-12). OK
- `tests/CMakeLists.txt:113-117`. OK

**One structural check the brief did not state, done anyway.** The brief says to append to
`test_notchcontroller.cpp` "OUTSIDE any anonymous namespace". Brace depth was computed across the
whole file: it is 0 at EOF and 0 before `:309`, so the anon namespace opened at `:11` (which
declares `Harness`) has closed, and the append lands at file scope with both `Harness` and
`Recorder` visible. Confirmed by the fact that it compiles.

**Line endings.** `test_notchcontroller.cpp` is 100 % CRLF; a heredoc append wrote LF and produced
a mixed-ending file. Normalised to CRLF before proceeding — `git diff --stat` then showed `87 +`
and no phantom whole-file rewrite. Every other edit was made through a Python script opened with
`encoding="utf-8", newline=""`, detecting and preserving each file's own newline (repo rule 6).

---

## 3. RED

`cmake --build build --config Release` with the three tests added and no production change:

```
tests\test_notchcontroller.cpp(3793,13): error C2039: 'origin': is not a member of 'NotchController::SnapshotNotch'
tests\test_notchcontroller.cpp(3793,13): error C2660: 'testing::internal::EqHelper::Compare': function does not take 3 arguments
tests\test_notchcontroller.cpp(3800,13): error C2039: 'origin': is not a member of 'NotchController::SnapshotNotch'
tests\test_notchcontroller.cpp(3800,13): error C2660: 'testing::internal::EqHelper::Compare': function does not take 3 arguments
tests\test_notchcontroller.cpp(3826,66): error C2838: 'SoundcheckReplace': illegal qualified name in member declaration
tests\test_notchcontroller.cpp(3826,66): error C2065: 'SoundcheckReplace': undeclared identifier
tests\test_notchcontroller.cpp(3835,5):  error C2838: 'SoundcheckReplace': illegal qualified name in member declaration
tests\test_notchcontroller.cpp(3835,5):  error C2065: 'SoundcheckReplace': undeclared identifier
tests\test_gui_wiring.cpp(1391,50):      error C2838: 'SoundcheckReplace': illegal qualified name in member declaration
tests\test_gui_wiring.cpp(1391,50):      error C2065: 'SoundcheckReplace': undeclared identifier
```

Exactly the two errors the brief predicted.

---

## 4. GREEN

Headers changed, so a full reconfigure.

```
$ cmake -B build -G "Visual Studio 18 2026" -A x64
-- ASIO SDK found at .../lane-m-soundcheck-0915/external/asiosdk
-- Configuring done (5.0s)
-- Generating done (1.1s)
-- Build files have been written to: .../lane-m-soundcheck-0915/build
```

```
$ cmake --build build --config Release
  HandsFreeTests.vcxproj -> ...\build\tests\Release\HandsFreeTests.exe
```

A second `cmake --build build --config Release` immediately after prints **zero** lines matching
`error C` or `warning` — the build output is pristine, no new warnings introduced.

```
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter='NotchControllerSoundcheck.*:GuiWiring.SoundcheckReplaceReachesTheLogAsItsOwnReason'
[ RUN      ] GuiWiring.SoundcheckReplaceReachesTheLogAsItsOwnReason
[       OK ] GuiWiring.SoundcheckReplaceReachesTheLogAsItsOwnReason (0 ms)
[ RUN      ] NotchControllerSoundcheck.SnapshotCarriesOrigin
[       OK ] NotchControllerSoundcheck.SnapshotCarriesOrigin (0 ms)
[ RUN      ] NotchControllerSoundcheck.SoundcheckReplaceIsItsOwnClearReason
[       OK ] NotchControllerSoundcheck.SoundcheckReplaceIsItsOwnClearReason (0 ms)
[==========] 3 tests from 2 test suites ran. (0 ms total)
[  PASSED  ] 3 tests.
```

Full suite:

```
$ cd build && ctest -C Release
585/585 Test #585: logstats_fixture .............   Passed    0.10 sec

100% tests passed, 0 tests failed out of 585

Total Test time (real) =  44.69 sec
```

**585, not the brief's 577.** The brief's 577 predates Tasks 2-3 and their two fix rounds; HEAD
`23c99f1` was 582, and this task adds 3. 582 + 3 = 585. The brief marked the number ESTIMATE.

---

## 5. Mutation evidence

Four mutations, each reverted immediately after.

**(a) Drop `n.origin` from the aggregate initialiser** — the mutation the brief names as the one
thing not to get wrong:

```
src\app\NotchController.cpp(578,43): error C2679: binary '=': no operator found which takes a
right-hand operand of type 'initializer list' (or there is no acceptable conversion)
```

Red, in all three targets (`HandsFree`, `HandsFreeSnapshot`, `HandsFreeTests`).

> **Finding, worth recording.** The brief's stated hazard — "brace elision would silently shift
> channel/index by one field" — does **not** apply at this particular position. `Origin` is a
> **scoped** enum (`enum class Origin`, `NotchController.h:62`), so `(std::uint8_t) c` has no
> implicit conversion to it and the omission is a hard compile error, not a silent re-bind. The
> hazard is real for a field of a type the neighbours convert to (another `float`, or another
> `std::uint8_t` inserted between `channel` and `index`) — it just is not what would have happened
> here. The guidance was followed regardless; the note is so a later reader does not over-trust the
> compiler for a differently-typed field.

Because (a) is a compile error rather than a test failure, a sharper mutation was run to prove
`SnapshotCarriesOrigin` itself has teeth:

**(b) Hardcode the field to the wrong value** (`Origin::Detector,` instead of `n.origin,`) —
compiles clean (0 `error C`), and the test fails at runtime:

```
    Which is: 4-byte object <02-00 00-00>
[  FAILED  ] NotchControllerSoundcheck.SnapshotCarriesOrigin (1 ms)
```

**(c) Remove the `reasonName` case** — compiles clean (0 `error C`); the reason falls through to
`"unknown"`:

```
[       OK ] NotchControllerSoundcheck.SnapshotCarriesOrigin
[       OK ] NotchControllerSoundcheck.SoundcheckReplaceIsItsOwnClearReason
[  FAILED  ] GuiWiring.SoundcheckReplaceReachesTheLogAsItsOwnReason
```

Note which two stayed green: the reason-name defect is invisible to the controller tests. This is
why B-3 puts that test in `test_gui_wiring.cpp` — it is the only place `notchEventToVarForTest` is
reachable.

**(d) The logstats fixture**, run with the expectation flipped to 0:

```
$ python tools/logstats.py tests/fixtures/session-sample.jsonl ... --expect-soundcheck-replaced 0
EXPECT FAILED: soundcheck-replaced 1 != 0
exit 1
```

`SoundcheckReplaceIsItsOwnClearReason` was not separately runtime-mutated: it cannot compile at all
without the enumerator (shown in section 3), and `EXPECT_NE (last.reason, ...::Manual)` is what
would catch a fold-into-Manual, which is likewise a compile-level distinction.

---

## 6. The fixture — the four unchanged expectations RE-DERIVED, not assumed

The brief demands this (lane G m-A). Read off `tools/logstats.py`, not off the old command line.

New lines, inserted at lines 15 and 16, **before** `session_end` at `t: 60000.0`:

```json
{"ev":"notch_set","t":50000.0,"slot":0,"lane":0,"index":5,"hz":700.0,"q":30,"depth_db":-9,"origin":"soundcheck"}
{"ev":"notch_clear","t":52000.0,"slot":0,"lane":0,"index":5,"hz":700.0,"origin":"soundcheck","reason":"soundcheck_replace","age_ms":2000.0}
```

- `(slot, lane, index) = (0, 0, 5)`. Existing triples are `(0,1,0)`, `(0,0,0)`, `(0,1,1)`, `(0,0,2)`
  — no collision, so `open_by_key` (`logstats.py:43`) pairs the clear with the right set.
- `hz = 700.0`. Existing: 1007.8, 2437.5, 1007.8, 482.0. Nearest distance is 218 Hz to 482.0 — far
  more than one bin (23.4375). So `groups` (`:95-102`) gains its own group of count 1 and
  **`recurrence-max` stays 2** (the 1007.8 pair).
- **`verdicts` stays 3** — `judged` (`:104`) counts only `verdict in ("good","false")`; the new
  notch has no `verdict` event, so it is unjudged. **`false` stays 1.**
- **`retunes` stays 2** — no `notch_retune` was added; `n["retunes"]` for the new record is 0.
- **`notches` 4 -> 5** — one `notch_set` added.
- `t` 50000/52000 sits after the last existing event (45400) and before `session_end` (60000).

Standalone run, before the C++ build:

```
$ python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-notches 5 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2 --expect-soundcheck-replaced 1
  # slot lane       hz origin       depth   deep  rt     held verdict  cleared by
  1    0    R   1007.8 detector      -6dB  -12dB   2    40.4s -        auto_release
  2    0    L   2437.5 detector     -18dB  -18dB   0     2.0s false    verdict_false
  3    0    R   1007.8 detector     -18dB  -18dB   0    40.0s good     still active
  4    0    L    482.0 preset       -12dB  -12dB   0    30.0s good     still active
  5    0    L    700.0 soundcheck    -9dB   -9dB   0     2.0s -        soundcheck_replace

recurrence (Hz within one bin, count of placements):
    1007.8 Hz  x2

notches 5  retunes 2  judged 3  false 1  false-rate 33%  unjudged 2 (40%)
soundcheck replaced 1 notch(es) from an earlier run
EXIT=0
```

Every re-derived number matches. `logstats_fixture` is ctest #585 and passes.

---

## 7. Step 6 — the duplicated-literal grep

```
$ grep -rn "0\.1f\|6000\.0\|10000\.0\|kSoundcheckMaxPeak\|kTrustedHighHz" src/ | grep -v "^src/dsp/SoundcheckSignal\|^src/dsp/LoopGainEstimator"
src/app/NotchController.h:132:    static constexpr double kReleaseStepMs    = 10000.0;
src/dsp/SoundcheckCandidates.cpp:149:        if (hz < SoundcheckSignal::kSweepLowHz || hz > LoopGainEstimator::kTrustedHighHz)
src/dsp/SoundcheckCandidates.h:133,172,175:   (comments naming kTrustedHighHz)
src/gui/RtaProcessing.h:46:        case AverageMode::S0_1: tauSeconds = 0.1f;  break;
src/gui/RtaProcessing.h:74,80,81:             third-octave band centre tables
src/gui/SpectrumView.cpp:64:                  spectrum grid decade lines
src/gui/SpectrumView.h:74:                    static constexpr float kDefaultHighHz = 16000.0f;
```

**Clean.** The only real use site, `SoundcheckCandidates.cpp:149`, is an alias pair exactly as the
brief requires. Everything else is pre-existing and unrelated: lane G's release step, the RTA
averaging time constant, the RTA third-octave band table, and SpectrumView's grid.

The grep as written is over-broad — bare `0.1f` / `10000.0` match any float anywhere in `src/`, and
the `grep -v` only excludes files whose path starts with those two prefixes (it does not exclude
`SoundcheckCandidates`). It produced no false negative here, but a later runner should not read its
non-empty output as a failure without inspecting each line.

---

## 8. Self-review

- **Additive only.** The `src/` diff is 3 functional lines. `SoundcheckReplace` is appended LAST in
  `ClearReason`, so no existing enumerator's underlying value changes — nothing persisted or logged
  by an older build is reinterpreted. `origin` has a default member initialiser, so any
  `SnapshotNotch` not from the one initialiser site still gets `Origin::Detector`.
- **The one initialiser site is the only one.** `grep -rn SnapshotNotch src/ tests/ tools/` finds
  the struct, `SnapshotBuffer::notches`, the initialiser, and three read-only consumers
  (`SpectrumView.cpp:878`, `SpectrumView.h:192`, `NotchListPanel.h:88` comment) that access members
  by name. No `sizeof(SnapshotNotch)`, no `memcpy` of the array, no `static_assert` on layout.
- **No behaviour change.** `setNotch`, `clearNotch`, the release ladder, `pushClearLocked`, the
  auto-release gate and every existing `ClearReason` path are byte-identical. All 582 pre-existing
  tests still pass.
- **DSP safety.** No filter coefficient, gain stage, buffer or clamp is touched. Expected level
  change: **0 dB**, as the brief states. Nothing reaches the audio callback.
- **Encoding.** All eight changed files: no BOM, valid UTF-8, non-ASCII byte counts unchanged from
  HEAD. `tools/logstats.py` still opens the log with `encoding="utf-8"` (`:19`) — untouched. Every
  edit script used explicit UTF-8 (repo rule 6).
- **Build hygiene.** `build/` only; no `build-*` variant, no build directory staged.
- **Git.** No `git stash` (there is a pre-existing `stash@{0}` from another session,
  `stray-c1-subagent-writeback-948d0e` — left strictly alone). No `git add .` / `-A`; the eight
  paths were named explicitly and `git diff --cached --name-only` confirmed the set before
  committing. `.superpowers/sdd/.gitignore` was removed before staging and does not exist now.
  `git status` shows only the pre-existing untracked SDD artefacts.

---

## 9. Concerns

1. **`ClearReason` has no switch outside `reasonName`.** `grep -rn "switch (r)" src/` finds exactly
   two, both in `MainComponent.cpp` (`:68` reasonName, `:87` retuneReasonName). So adding an
   enumerator could not break a second exhaustive switch — but it also means **nothing warns** when
   a future enumerator forgets its name: `reasonName` ends in `return "unknown";`, not a
   `static_assert` or `jassertfalse`. Mutation (c) is the only thing standing between a future
   `ClearReason` and a silently mislabelled log line. Out of scope here; worth a lane-D note.
2. **Brief imprecisions, recorded above, none blocking:** the brace-elision hazard at this exact
   position is a compile error because `Origin` is a scoped enum (section 5a); the test count is
   585 not 577 (section 4); the Step 6 grep is over-broad (section 7).
3. **`ClearReason::SoundcheckReplace` currently has no production caller.** It exists for Task 6.
   Same for `SnapshotNotch::origin` — the only reader today is the new test. Both are inert until
   lane M's soundcheck controller lands, which is exactly what the "additive only" constraint asks
   for, but it means the *policy* (which notches get replaced, and when) is still entirely
   unverified. Nothing here proves the replace behaviour is correct — only that the two hooks carry
   their values intact.
