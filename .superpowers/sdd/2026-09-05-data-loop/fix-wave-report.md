# Lane D fix wave — report

**Branch** `feat/data-loop` · **FIX_BASE** `48f3db7` · **HEAD** `ae6ddbe`
**Implementer** opus (Claude Fable 5.1) · **Date** 2026-09-05

## Status

All 20 findings addressed (V1 V2 V3, I-1 I-2 I-3, M-1 M-2 M-3 M-4 M-5 M-6 M-7
M-8 M-9 M-10 M-11 M-12, T2, T6, A-11). Nothing skipped. Full suite green at
**432/432** (was 430; the wave adds 2 tests).

## Commits

| SHA | Subject |
|---|---|
| `e468e76` | fix(lane-D): verdict reset on re-place, silent-failure paths, log gaps |
| `9b59a1f` | test(lane-D): make the teardown test place notches, pin the scorer analytically |
| `ae6ddbe` | docs(lane-D): release status, the two age_ms clocks, real log sizes, A-11 |

## Binding constraints — held

- **No audio-thread code.** `git diff --name-only 48f3db7..HEAD` touches no
  `NotchController.cpp`, `NotchChain`, `AudioEngine`, `CandidateScorer`,
  `Detector`, `Biquad` or `PeakinessAnalyzer` production file. 0 dB.
- **No detection decision changed.** `processSpectrumForDetection`,
  `placeConfirmed`, `setWidth` and the scorer arithmetic are byte-identical;
  the only scorer-side change is a TEST that recomputes its formula.
- The sink is still never called under `modelMutex_` (the outbox path is
  untouched); `MainComponent` still never starts the logger in its ctor;
  `JUCE_APPLICATION_VERSION_STRING` still appears only in `main.cpp`.
- Every new test carries a comment naming the production change that turns it
  red.

## What changed

### Code — `e468e76`

| Finding | Change |
|---|---|
| I-1 | `NotchListPanel` remembers the previous refresh's clock reading (`lastRefreshMs_`, -1 before the first refresh). An EXISTING sighting whose `lastSeenMs` predates it was absent for a whole refresh, i.e. cleared and re-placed, so `buttons_[key].state` resets to `Verdict::None`. The age / first-seen carry-over is untouched (R-2). |
| I-2 | The event sink is wrapped in `if (sessionLogger_.isActive())`, so `notchEventToVar` builds nothing (three rounded 1025-value spectra) when there is no file to write to. |
| I-3 | New public `MainComponent::showMessage (const juce::String&)` sets `panelMessage_` and calls `refreshStatus()` — the same route `devicePanel_.onMessage` uses. `main.cpp` now checks `startSessionLog()` and reports `LOG: khong ghi duoc <path>` (ASCII literal) plus `juce::Logger::writeToLog`. |
| M-1 | `SessionLogger::writeLineNow` records refused writes in a sticky `std::atomic<bool> writeFailed_` (both writes attempted, no short-circuit); `session_end` carries `write_failed` next to `dropped_events`. Reset in `start()`. |
| M-3 | `ref_age_ms` moved inside the `hasRef` guard. |
| M-4 | `loadPreset` sums the `adoptPreset()` returns and logs `preset_load { file (name only), adopted, skipped }`. |
| M-7 | `SessionLogger.h` invariant sentence rewritten: `session_start` / `session_end` bypass the deque, `lines == log() calls - droppedEvents() + 2`, `droppedEvents()` is authoritative, the in-file `dropped_events` can under-count a producer that loses the close race; `write_failed` documented. |
| M-8 | The global `tuning` lambda casts `rise_ms`, `q`, `depth_db` to `double`. |
| M-9 | `ensureButtonsFor` lost its unused `notch` parameter (header, definition, call site, and the `ignoreUnused`). |
| M-11 | `tools/snapshot.cpp` null-checks `goodButtonForTest` / `falseButtonForTest`, prints to `std::cerr` and returns 1. |
| T6 | `startSessionLog` logs a `mode` event with `engine_.getMode()` right after a successful `sessionLogger_.start`. |
| M-6 (code half) | The "clone" wording in `SessionLogger.h` / `.cpp` now says one-level `DynamicObject` copy, not `var::clone()`. |

### Tests — `9b59a1f`

- **V1** `GuiWiring.DestroyingTheAppWhileADetectorIsPlacingNotchesDoesNotCrash`
  rewritten. Root cause of the old no-op: it fed a **steady** 1 kHz tone for
  ~21 ms. The rise axis divides now by the newest frame at least
  `0.45 x riseReferenceMs` old, so a tone already at full level in that frame
  gives `rNorm = 0` and nothing can ever confirm — more wall time alone would
  not have fixed it. The new feed is a real howl: ~120 ms of quiet noise
  (amplitude 0.05) to lay down the rise history and warm the baseline EMA,
  then a loud 1 kHz tone, both written 3 ms per hop (the scorer's clock is
  wall clock and the tap ring holds only 16 hops). Per round:
  `setRiseReferenceMs (100)`, `setPersistenceBlocks (1)`,
  `setDetectionActive (true)`, `start()`, then `app` is destroyed with the
  thread running. Each round's `session-*.jsonl` is parsed afterwards.
- **V2** new `CandidateScorerBreakdown.ScoreMatchesTheDocumentedFormulaOnHandBuiltFrames`
  — analytic, hand-built constant frames (A 2.0 at dt 0, B 2.0 at dt 3000,
  C 2.0 at dt 100, giving a known EMA and a known qualifying reference at age
  3100 ms), peakiness 55 so `pNorm = 0.5` exactly. Case 1: `now = 8.0`, rise
  and novelty saturate, penalty 1 and then `locked = {300}` giving 0.5.
  Case 2: `now = 2.6`, so `rNorm = 0.6` and `mNorm ~ 0.507` are both
  **unclamped** and every axis is pinned by value. Both cases also assert
  `EXPECT_EQ (scorer.scoreCandidate (...), b.score)`. The old test keeps its
  real job under the honest name `DetailedScoreIsTheScalarOverloadsSourceOfTruth`.
- **T2** the `kHarmonicPenalty` expectation keys off `cand.frequencyHz` being
  inside 1.4x..4.1x of the locked 300 Hz.
- **I-1** new `NotchListPanelVerdict.ReplacedNotchAtTheSameIdentityStartsUnjudged`:
  GOOD on 987 Hz, `clearNotch`, republish, refresh (row gone),
  `setNotch (0, 0, 987, ...)`, republish, refresh — verdict `None`, both
  buttons visible, age still 500 ms (carry-over preserved). A row that never
  left keeps its verdict.
- **T6** `ModeAndTuningChangesAreLogged` asserts the FIRST `mode` line is
  `bypass` (the session's starting mode), that there are exactly two, and that
  the last is `auto`.
- **M-1** `SessionLogger.StartWritesHeaderFirstStopWritesEndLastEveryLineParses`
  asserts `write_failed` is present and false.
- **M-5** `Recorder` declared before `Harness` / `StereoHarness` at all 8
  sites, with the reason stated once on the `Recorder` struct.
- **M-10** `statusW360` renamed `statusWNarrowest`.

### Docs — `ae6ddbe`

V3 (roadmap row D now says the release runs after and has not been bumped);
M-2 (the two `age_ms` clocks, the `preset_load` row, the
`session_end` / `write_failed` row, mode-at-session-start); M-12 (measured
sizes, see the deviations below, corrected in both KY-THUAT section 7 and the
design spec); M-6 (memory note sections 1-3); A-11 (plan amendment table).
`GIOI-THIEU.md` picks up the two user-visible changes (a re-placed notch comes
back unjudged, and the status line reports a log that could not be opened).
Suite count 430 -> 432 wherever it was stated. All UTF-8, no BOM.

## Verification

### V1 per-round `notch_set` counts, three runs

```
build/tests/Release/HandsFreeTests.exe --gtest_filter=GuiWiring.Destroying* --gtest_repeat=3

[ INFO ] notch_set per round: 14 22 26 22 26 16 16 16 16 16 22 22 24 24 16 16 26 14 14 16 (total 384, rounds with >=1: 20)
[   OK ] GuiWiring.DestroyingTheAppWhileADetectorIsPlacingNotchesDoesNotCrash (8199 ms)
[ INFO ] notch_set per round: 14 22 20 22 22 24 26 22 16 20 14 14 28 28 18 30 16 24 16 28 (total 424, rounds with >=1: 20)
[   OK ] GuiWiring.DestroyingTheAppWhileADetectorIsPlacingNotchesDoesNotCrash (8133 ms)
[ INFO ] notch_set per round: 22 26 24 16 22 16 16 22 28 26 28 24 14 18 22 24 16 16 22 16 (total 418, rounds with >=1: 20)
[   OK ] GuiWiring.DestroyingTheAppWhileADetectorIsPlacingNotchesDoesNotCrash (8179 ms)
[ PASSED ] 1 test.   (each repeat)
```

Floor asserted: total >= 10, and at least one round with a Set. Observed
384-426 total, 14-30 per round, **every** round >= 14, ~8.2 s per run. Every
round's file is also asserted to end in `session_end` — the actual proof that
the logger outlived the controllers' flush.

### Focused ctest

```
ctest -C Release --output-on-failure -R "CandidateScorer|NotchListPanel|SessionLogger|GuiWiring|NotchController"
...
102/103 Test #430: SessionLogger.LogAfterStopIsANoOpAndNotCounted ...... Passed  0.06 sec
103/103 Test #431: SessionLogger.DestructionWithoutStopWritesSessionEnd  Passed  0.06 sec

100% tests passed, 0 tests failed out of 103

Total Test time (real) =  15.63 sec
```

### Full gate, run before the last commit

```
ctest -C Release
...
432/432 Test #432: logstats_fixture ................................... Passed  0.10 sec

100% tests passed, 0 tests failed out of 432

Total Test time (real) =  35.10 sec
```

### Red-without-the-fix check (I-1)

`lastRefreshMs_ >= 0.0` temporarily disabled, rebuilt, and the new test fails:

```
tests\test_notchlistpanel.cpp(560): error: Value of: panel.falseButtonForTest (0)->isVisible()
  Actual: false
Expected: true
[  FAILED  ] NotchListPanelVerdict.ReplacedNotchAtTheSameIdentityStartsUnjudged
```

Restored and rebuilt green.

### Code and docs agree

```
grep -n "write_failed\|preset_load" src/app/*.cpp docs/KY-THUAT-CHONG-HU.md

src/app/MainComponent.cpp:873:        auto v = SessionLogger::makeEvent ("preset_load");
src/app/SessionLogger.cpp:150:    end.getDynamicObject()->setProperty ("write_failed",
docs/KY-THUAT-CHONG-HU.md:315:| `preset_load` | ... `file` ... `adopted` ... `skipped` ... |
docs/KY-THUAT-CHONG-HU.md:316:| `session_end` | ... `dropped_events` ..., `write_failed` ... |
```

### Git

```
git log --oneline 48f3db7..HEAD
ae6ddbe docs(lane-D): release status, the two age_ms clocks, real log sizes, A-11
9b59a1f test(lane-D): make the teardown test place notches, pin the scorer analytically
e468e76 fix(lane-D): verdict reset on re-place, silent-failure paths, log gaps

git status --short
?? .superpowers/sdd/2026-08-27-next-wave/review-ec4e3a1..f326543.diff
?? .superpowers/sdd/2026-08-27-next-wave/task-T1-brief.md
?? .superpowers/sdd/2026-08-27-next-wave/task-T1-report.md
```

Nothing but pre-existing `.superpowers/` working files. `build/` and `shots/`
are gitignored and were never staged.

### Screenshot

`build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast` runs clean
after the M-11 change. `shots/console-live.png` read back: the VERDICT column
shows GOOD / FALSE / two live buttons, no truncation, no mojibake.

## Deviations from the brief — all deliberate, none skipped

1. **M-12 numbers.** The brief said ~23 KB per `notch_set` and 7-14 MB for a
   3-hour show. **Measured** on the teardown test's own logs (76 real Set
   lines, stereo, `now` + `ref` + `other_lane_now`): 19 511-19 719 bytes,
   average 19 639. The docs therefore state **19.5-19.7 KB measured**, note
   ~23 KB as the ceiling if every value spends all three significant figures,
   ~40 KB for a LINK pair, and **6-14 MB** for the show. Writing 23 KB as a
   flat fact would have been a number nobody had checked.
2. **M-6 section 1 example.** The brief's illustrative value was
   `0.012345678918063641`. Recomputed here: `(double) 0.0123456789f` is
   `0.012345679104328156`, 20 characters. The doc carries the recomputed one.
   The lesson itself (round to 3 significant figures before the `var`) stands.
3. **V2 scope.** The brief's single case has BOTH `rNorm` and `mNorm` clamped
   at 1 (`log (8 / 1.288) / log 4 = 1.32 -> 1`), so on its own it would pin
   only `pNorm` and the penalty. It is implemented exactly as asked, and a
   second case (`now = 2.6`) was added where nothing saturates, so the rise
   normaliser and the log-4 novelty scale are pinned by value too.
4. **Extra scope, small.** The design spec's own size estimate (12-18 KB) was
   corrected alongside KY-THUAT's so the two cannot disagree; `GIOI-THIEU.md`
   picked up I-1 and I-3 per the project's "user-visible behaviour updates
   both docs" rule; the suite count 430 -> 432 was fixed in the four places
   that stated it.

## Not done — why

Nothing. All 20 findings are implemented.

## Notes for whoever releases 1.1.2

- `CMakeLists.txt` is still at 1.1.1 and the roadmap row now says so. The
  release script bumps it; that row wants re-editing after the run.
- The teardown test writes ~20 real session logs per run into
  `%TEMP%\az-handsfree-sessionlog\teardown-*` (~400 KB per round, ~8 MB per
  run). It self-cleans each directory at the START of the next run, as the
  other lane-D tests already do, so the last run's files survive on disk until
  then.
