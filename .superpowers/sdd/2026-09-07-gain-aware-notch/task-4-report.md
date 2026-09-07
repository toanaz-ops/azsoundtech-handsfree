# Task 4 report — the depth ladder in `NotchController`

Branch `claude_desk/lane-g-brainstorm-sdd-f3c568`, worktree
`D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-g-brainstorm-sdd-f3c568`.
Commit **`cc2e77f`** — `feat(app): NotchController depth ladder — model fields,
−24 clamp, pushRetuneLocked, Retune event`. One commit; the brief's steps did
not split cleanly (the header, the .cpp and the tests only compile together).

## Expected level change

**0 dB for detector placements** — nothing calls the retune path in production
yet (Tasks 5–7 do), and no shipped preset asks for more than −24 dB
(`presets/Speech.json` −18, `presets/Music.json` −10). The one shipped
behaviour change is Q12's clamp: a hand-edited preset asking for, say, −40 dB
is now cut to −24 dB, i.e. **up to 16 dB shallower** than 1.1.3, and
`adoptPreset` logs one line saying so.

## Implemented

`src/app/NotchController.h`

- `#include <optional>`, `#include <utility>`.
- Lane-G constant block after `kDefaultNotchDepthDb`: `kDepthLadderDb[4]`,
  `kDepthLadderSize`, `kDepthStepDb`, `kMaxDepthDb`, `kDeepenAfterMs`,
  `kSteepRiseRatio`, `kReleaseFirstMs` (= `kAutoReleaseMs`), `kReleaseStepMs`,
  `kMemoryTtlMs`, `kMemoryEntriesPerLane`, `kRiskFreezeFraction`.
- `enum class RetuneReason : std::uint8_t { Deepen, Release, Reclamp, Ceiling }`.
- `static double nextDeeperRungDb (double, double)` /
  `static double nextShallowerRungDb (double)`.
- `NotchEvent::Kind` gains `Retune`; the struct gains `retuneReason`,
  `fromDepthDb`, `riseRatio`.
- Test accessors `depthDbForTest`, `deepestDbForTest`, `quietMsForTest`,
  `activeForTest`, `retuneForTest`, `setRingRiskOverrideForTest`.
- `SnapshotNotch` gains `deepestDb`; `SnapshotBuffer` gains
  `releaseFrozen = false`.
- `ModelNotch` gains `deepestDb`, `stageChangedAtMs`, `quietMs`,
  `releasedSteps`, `ceilingDb`.
- Private `bool pushRetuneLocked (...)` and `double ceilingDbFor (const
  ModelNotch&) const`; member `std::optional<std::pair<bool, float>>
  ringRiskOverrideForTest_`.

`src/app/NotchController.cpp`

- `#include <optional>`, `#include <utility>`.
- `nextDeeperRungDb` / `nextShallowerRungDb` / `ceilingDbFor` after
  `setWidth`.
- `setNotchImpl`: `const double depth = std::max (depthDB, kMaxDepthDb);`
  computed BEFORE validation (so `+3.0` is still refused), then used for the
  command, the model, and the event. All five ladder fields initialised inside
  the lock; `ceilingDb` is `quiet_NaN()` for `Origin::Detector` and the
  clamped `depth` for every other origin (Q8).
- `pushRetuneLocked` immediately after `pushClearLocked`.
- `adoptPreset`: `clamped` counter incremented only when the notch was
  actually applied AND `lane == firstLane` AND `p.depthDB < kMaxDepthDb`
  (M-5); one `juce::Logger::writeToLog` line when `clamped > 0`.
- Snapshot notch gather carries `(float) n.deepestDb`.
- `scored.riseRatio = pc.breakdown.riseRatio;` in `placeConfirmed` (B-5).
- The six test accessors next to `liveMsForTest`.

`tests/test_notchcontroller.cpp` — 11 new `NotchControllerLadder.*` tests,
verbatim from the brief.

## Tests and results

TDD evidence.

**RED** — tests appended first, `cmake --build build --config Release`:

```
error C2039: 'nextDeeperRungDb': is not a member of 'NotchController'
error C2039: 'nextShallowerRungDb': is not a member of 'NotchController'
error C2039: 'depthDbForTest': is not a member of 'NotchController'
error C2039: 'deepestDbForTest': is not a member of 'NotchController'
error C2039: 'quietMsForTest': is not a member of 'NotchController'
error C2039: 'activeForTest': is not a member of 'NotchController'
error C2039: 'retuneForTest': is not a member of 'NotchController'
error C2039: 'Deepen': is not a member of 'NotchController'
error C2039: 'Reclamp': is not a member of 'NotchController'
error C3083: 'RetuneReason': the symbol to the left of a '::' must be a type
error count exceeds 100; stopping compilation
```

**GREEN** — after the implementation, `cmake --build build --config Release`
completed with exit 0 and no error lines. The only warnings emitted are the
pre-existing ones in `HandsFree.vcxproj` (C4244 float→int in GUI code, C4324
`LockFreeRingBuffer` alignment padding ×4, C4996 `juce::Displays::Display::
userArea`). No warning names `NotchController`.

Focused: `build/tests/Release/HandsFreeTests.exe --gtest_filter=NotchController*`

```
[----------] 11 tests from NotchControllerLadder (6 ms total)
[==========] 78 tests from 14 test suites ran. (1392 ms total)
[  PASSED  ] 78 tests.
```

Full gate: `cd build && ctest -C Release`

```
100% tests passed, 0 tests failed out of 484

Total Test time (real) =  39.25 sec
```

473 before this task, 11 new, 484 now. No pre-existing test moved.

## Deviations from the brief

1. **Line numbers.** The brief's anchors were taken before Tasks 1–3 landed;
   every edit was located by grepping the quoted text instead. No semantic
   difference.
2. **Two comments shortened.** The brief's `kRiskFreezeFraction` and
   `activeForTest` comments cite `SpectrumView.h:239`, `SpectrumView.h:45` and
   `NotchController.cpp:195-212`. Line numbers in a comment go stale the next
   time anything above them moves, so the file names were kept and the line
   numbers dropped. Wording otherwise verbatim.
3. **One commit, not two.** The brief's Step 7 has a single commit anyway; the
   message is the one this task was given, not the brief's shorter one.
4. **Nothing else.** No design choice was improvised — in particular
   `pushRetuneLocked` deliberately does NOT touch `deepestDb`,
   `stageChangedAtMs`, `quietMs` or `releasedSteps`, exactly as the brief's
   body specifies. Tasks 5–7 own that bookkeeping. The brief's own snapshot
   test depends on it (`deepestDb` stays −18 after a release to −6).

## Files

- `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-g-brainstorm-sdd-f3c568\src\app\NotchController.h`
- `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-g-brainstorm-sdd-f3c568\src\app\NotchController.cpp`
- `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-g-brainstorm-sdd-f3c568\tests\test_notchcontroller.cpp`

`tests/test_gui_wiring.cpp` needed no change: `SnapshotNotch` is
aggregate-initialised in exactly one place in the tree
(`NotchController.cpp`, the snapshot gather), and `NotchEvent::Kind` is never
switched on — `MainComponent.cpp:562` uses a ternary, so adding `Retune` did
not break a `-Wswitch`.

## Self-review

**Lock audit (the one the task asked for).**

`grep -rn "pushRetuneLocked" src tests` finds exactly one call site outside
the declaration and definition: `NotchController.cpp:590`, inside
`retuneForTest`, on the line after `const std::lock_guard<std::mutex> lock
(modelMutex_);`. Nothing in production calls it yet, as intended.

Everything `pushRetuneLocked` itself calls is lock-free or already
lock-holding:

- `lanes_[0].detector.getSampleRate()` → `Detector.cpp:37-40`, a single
  `sampleRate_.load (std::memory_order_relaxed)`. No mutex.
- `outbox_.push_back` → plain `std::vector`, guarded BY `modelMutex_`.
- `pushEventLocked` → declared `// modelMutex_ HELD`; its body touches
  `eventSink_`, `eventOutbox_` and `droppedEvents_` and takes no lock.

It never touches `snapshotMutex_`. `grep -n "snapshotMutex_"
src/app/NotchController.cpp` returns two sites (the publish block in
`runOnce`, and `copySnapshot`), neither reached from this task's code, and
neither nested inside a `modelMutex_` scope — unchanged from before.

**Invariant 1.** `grep -n "NotchCommandType::Set" src/app/NotchController.cpp`
returns exactly two constructions: `:205` in `setNotchImpl`, which carries
`(float) depth` (the `std::max (depthDB, kMaxDepthDb)` result), and `:293` in
`pushRetuneLocked`, which is unreachable unless `newDepthDb >= kMaxDepthDb`.
No Set deeper than −24 dB can leave this controller, whatever the Origin.

**Slot reuse.** Inside `setNotchImpl`'s lock, the writes are `frequency`, `Q`,
`depthDB`, `origin`, `active`, `lockedAtMs`, `lastDetectedMs`, `deepestDb`,
`stageChangedAtMs`, `quietMs`, `releasedSteps`, `ceilingDb` — every member
`ModelNotch` declares. `pushClearLocked` still writes only `n.active = false`
and still reads `n.depthDB` for the Clear event, so `activeForTest` reads the
flag (`NotchController.cpp`, `activeForTest`) and the retained depth is
asserted by `ActiveForTestReadsTheFlagNotTheRetainedDepth`.

**`git show --stat cc2e77f`** — 3 files, 599 insertions, 11 deletions.
`.superpowers/sdd/.gitignore` was deleted before staging; `git status
--porcelain` shows only the untracked `.superpowers/sdd/2026-09-07-gain-aware-notch/`
directory, which was not staged.

**Independent verification.** A fresh adversarial verifier agent (no Edit/Write)
read the committed files rather than this report and returned **all six claims
CONFIRMED**, with file:line evidence: the two Set emit sites are
`NotchController.cpp:205` (clamped at `:189`) and `:293` (refused at `:288`);
`AudioEngine.cpp:441` is a consumer switch-case, not an emitter. The only
`pushRetuneLocked` caller is `NotchController.cpp:590`, one line after the
`modelMutex_` guard at `:589`. `grep -n "active.*=.*true"` returns exactly one
hit, `:217`, inside `setNotchImpl`'s re-init block — so every activation path
(detector, preset, soundcheck, manual) re-initialises the ladder. Its break
attempts on `clearAll` (`:325-331`), `setWidth`'s narrowing (`:82-97`), the
`adoptPreset` unwind (`:374-376`), `placeConfirmed` and `setSampleRate`
(`:1036-1049`) all failed to find a path leaving a `ModelNotch` active with
stale ladder state or emitting a Set deeper than −24 dB.

## Concerns for the next tasks

1. **`MainComponent::notchEventToVar` maps `Kind` with a ternary**
   (`MainComponent.cpp:562`: `e.kind == Ev::Kind::Set ? "notch_set" :
   "notch_clear"`). A `Retune` event today would be logged as `notch_clear`.
   Harmless while nothing emits one, but the FIRST task that calls
   `pushRetuneLocked` from production MUST fix that line in the same change,
   or `tools/logstats.py` will close each notch's record at its first 300 ms
   deepening (the brief's B-3).
2. **`ceilingDbFor`, `kSteepRiseRatio`, `kDeepenAfterMs`, `kMemoryTtlMs`,
   `kMemoryEntriesPerLane`, `kRiskFreezeFraction`, `stageChangedAtMs`,
   `quietMs`, `releasedSteps` and `ringRiskOverrideForTest_` are declared and
   unused.** They compile clean, but nothing yet proves `ceilingDbFor` returns
   what Q8 wants — its NaN branch has no test until Task 5 or 6 exercises it.
3. **`pushRetuneLocked` does not maintain `deepestDb`.** By design here, but
   it means the ladder's "deepest rung ever held" is only correct once a later
   task updates it on every deepen. Until then a `savePreset` reading
   `SnapshotNotch::deepestDb` records the PLACEMENT depth, not the deepest
   rung.
4. **Q8's slider rule is only half-built.** `ceilingDb` is stored correctly
   (NaN for Detector, the caller's depth otherwise), but nothing reads it, so
   moving the depth slider still has no effect on placed notches.

## Fix round 1

A reviewer raised two Important findings against `cc2e77f`. Both are fixed;
neither required a behaviour change to the shipped ladder logic itself --
`setNotchImpl` and `nextDeeperRungDb` were already correct, just untested and
under-documented.

**Important 1 — `ceilingDbFor` had no test and no accessor.**

- `src/app/NotchController.h:289-311` — added four TEST-ONLY accessors next
  to the existing lane-G block: `double ceilingDbForTest(channel, index)`
  (the RESOLVED ceiling `ceilingDbFor(n)` returns), `double
  rawCeilingDbForTest(channel, index)` (the stored `ModelNotch::ceilingDb`
  field itself -- NaN for Detector), `int releasedStepsForTest(channel,
  index)`, `double stageChangedAtMsForTest(channel, index)`.
- `src/app/NotchController.cpp:601-627` (after `retuneForTest`) — the four
  implementations, same lock-once-under-`modelMutex_` pattern as the existing
  accessors; `ceilingDbForTest` calls the private `ceilingDbFor()` while
  already holding the lock (that method itself takes no lock).
- `tests/test_notchcontroller.cpp` — new test
  `CeilingIsTheSliderForDetectorAndOwnDepthForPresetAndManual`: places a
  Detector notch and asserts `rawCeilingDbForTest` is NaN and
  `ceilingDbForTest == getNotchDepthDb()`, then calls `setNotchDefaults(30,
  -12)` and asserts the resolved ceiling now reads -12 (proves it tracks the
  LIVE slider, not a value captured at placement time); places a Manual notch
  at -9 and a Preset notch (via `adoptPreset`) at -15 and asserts both their
  raw and resolved ceilings equal their own depth and do NOT move when the
  slider is changed afterward.
- Extended `ReusingASlotResetsEveryLadderField` to cover all five re-init
  fields instead of two: the first tenant is now Manual at -24 (a non-NaN raw
  ceiling) and 250 ms of live time is advanced before reuse, so a stale
  `ceilingDb` or a stale `stageChangedAtMs` would be provably stale rather
  than accidentally matching the second tenant's own values. Added
  `releasedStepsForTest == 0` on both tenants -- honestly, this is a weak
  assertion given nothing in Task 4 can drive `releasedSteps` away from 0
  through the public API yet (that bookkeeping is Tasks 5-7's), so it proves
  "still 0" rather than "was reset from nonzero"; the `ceilingDb`
  (NaN-vs-`-24`) and `stageChangedAtMs` (must exceed the pre-reuse value)
  assertions are the ones that actually exercise the re-init path.

**Important 2 — `nextDeeperRungDb`'s contract comment was false past the
ceiling.**

- `src/app/NotchController.h:142-154` and
  `src/app/NotchController.cpp:123-140` — comment rewritten to state the
  actual contract: at the ceiling, `currentDb` is returned unchanged; deeper
  than the ceiling (`currentDb < ceilingDb`), the ceiling itself is returned,
  which is SHALLOWER than `currentDb` -- a step in the wrong direction for a
  caller that only ever deepens, which must compare the result against
  `currentDb` before sending it as a command. No code change: the function's
  arithmetic (`std::max(next, ceilingDb)`) was already correct; only the
  comment was wrong.
- `tests/test_notchcontroller.cpp` -- one new table row in
  `TheEffectiveLadderEndsOnTheCeilingItself`:
  `EXPECT_DOUBLE_EQ (NC::nextDeeperRungDb (-24.0, -10.0), -10.0);` with a
  comment naming the case. This passed on the FIRST build with no
  implementation change, confirming the bug was in the comment, not the code.

**Minors folded in.**

- `src/app/NotchController.cpp` (top of `pushRetuneLocked`) --
  `jassert (channel >= 0 && channel < kChannels && index >= 0 && index < kSlots);`
  Documents the caller's precondition (only reachable today via
  `retuneForTest`, which already range-checks before taking the lock).
- `src/app/NotchController.cpp` (top of `nextDeeperRungDb`) --
  `jassert (! std::isnan (ceilingDb));`, guarding against a caller passing a
  raw (unresolved) `ModelNotch::ceilingDb` instead of `ceilingDbFor()`'s
  result. Confirmed `jassert` is available before using it: `grep -rn
  jassert src` already had one use (`MainComponent.cpp:775`), and
  `NotchController.h` pulls in JUCE via `<juce_events/juce_events.h>`, which
  transitively includes `juce_core` (where `jassert` is defined) -- same as
  every other JUCE-using translation unit in this tree.

**TDD evidence (fix round 1).**

RED -- accessors temporarily removed via `git stash` of the header/cpp
changes, tests added, `cmake --build build --config Release`:

```
error C2039: 'rawCeilingDbForTest': is not a member of 'NotchController'
error C2039: 'ceilingDbForTest': is not a member of 'NotchController'
```

(repeated at each call site; 8 errors total, all the same two symbols)

GREEN -- `git stash pop` restored the accessor implementation,
`cmake --build build --config Release` completed with exit 0, no
`NotchController` warnings or errors (the emitted warnings are the
pre-existing `NotchListPanel.cpp`/`MainComponent.cpp` ones noted in the
original report).

Focused: `build/tests/Release/HandsFreeTests.exe --gtest_filter=NotchControllerLadder*`

```
[----------] 12 tests from NotchControllerLadder (8 ms total)
[==========] 12 tests from 1 test suite ran. (9 ms total)
[  PASSED  ] 12 tests.
```

Full gate: `cd build && ctest -C Release`

```
100% tests passed, 0 tests failed out of 485
Total Test time (real) =  79.04 sec
```

484 before this fix round, 1 new (`CeilingIsTheSliderForDetectorAndOwnDepthForPresetAndManual`),
485 now. No pre-existing test moved.

**Scope.** Only `src/app/NotchController.h`, `src/app/NotchController.cpp`
and `tests/test_notchcontroller.cpp` changed, per the fix-round rules. No
lock-order change: the four new accessors take `modelMutex_` exactly once,
same as every existing `*ForTest` accessor. No behaviour change to the
shipped ladder logic -- both findings were test-coverage and documentation
gaps, not bugs in `cc2e77f`'s code.

**Remaining concerns (unchanged from the original report).** Items 2-4 of
"Concerns for the next tasks" above still apply: `ceilingDbFor` is now
tested, but `kSteepRiseRatio`, `kDeepenAfterMs`, `kMemoryTtlMs`,
`kMemoryEntriesPerLane`, `kRiskFreezeFraction`, `stageChangedAtMs` (as a
ladder-progression input, not just a re-init target), `quietMs` and
`releasedSteps` are still unread outside this fix round's own accessors and
re-init path -- Tasks 5-7 own wiring them into the actual deepen/release
ladder.
