# Task 3 report — NotchController: ClearReason, NotchEvent, EventSink

## What was implemented

Exactly the brief's API, no more:

- `NotchController::ClearReason` (`Manual, ClearAll, AutoRelease, WidthChange, VerdictFalse, PartialApplyUnwind`), placed right after `enum class Origin`.
- `NotchController::SpectralContext` and `NotchController::NotchEvent` structs, `EventSink` alias, `kMaxPendingEvents = 64`.
- `setEventSink()`, `droppedEvents()`, and test accessors `pendingEventsForTest()`, `modelMutexIsFreeForTest()`, `failNextSetNotchOnLaneForTest()`.
- `clearNotch(channel, index, reason = Manual)`, `clearAll(reason = ClearAll)`, `adoptPreset(notches, skippedOut = nullptr)` — all existing call sites at their old arity keep compiling via the new defaults.
- `setNotch()` is now a thin wrapper over `setNotchImpl(..., const NotchEvent* scored)`; `scored` stays `nullptr` from every Task-3 call site (Task 4 will pass a pre-filled event for scored detector placements — `hasScore`/`ctx` are untouched here).
- `pushClearLocked()` takes a mandatory `ClearReason` (no default — every call site was forced to pick one by the compiler, per the brief's intent).
- `pushEventLocked()` — the cap/drop/no-sink logic, called under `modelMutex_` from both `setNotchImpl` and `pushClearLocked`.
- `flushEventOutbox()` — swaps `eventOutbox_`/`eventScratch_` under the lock, then invokes the sink over the scratch copy with the lock released (spec D-6 / spec test 9).
- `stop()` now always calls `flushEventOutbox()` after conditionally joining the thread (spec test 11 — flush must happen even if the thread never ran).
- `setWidth()`: narrowing block now passes `ClearReason::WidthChange`; added the widening branch (`newWidth > width_`) that calls `Detector::reset()`, zeroes `persistence`, and resets `previousBlockNowMs` for lanes coming back into scope (Lane S loose end A-9).
- `adoptPreset()` counts lane-1-named-on-mono-slot notches into `*skippedOut` when `firstLane >= width_`.
- Both internal unwind sites (`adoptPreset`'s partial-apply unwind, `placeConfirmed`'s partial-apply unwind) now pass `ClearReason::PartialApplyUnwind` instead of the old default.
- Auto-release in `runOnce()` step 3 passes `ClearReason::AutoRelease`; `runOnce()` step 4 now also calls `flushEventOutbox()`.
- Constructor reserves `eventOutbox_`/`eventScratch_` to `kMaxPendingEvents`.
- `MainComponent.cpp` needed NO changes — `clearAll()` (line 109) and `adoptPreset(notchesForSlot)` (line 578) both compile unchanged against the new default parameters.

## TDD evidence

**RED** — `cmake --build build --config Release --target HandsFreeTests` after appending the brief's tests verbatim (header untouched at this point... actually header was already edited before this build per the natural flow: tests appended, then header edited, .cpp not yet touched) produced compile errors exactly where expected — `pushClearLocked`/`clearNotch`/`clearAll`/`adoptPreset` arity mismatches and `model_`/`slotId_`/`width_`/`modelMutex_` "illegal reference to non-static member" errors (the .cpp file still declared the old function bodies with stale signatures against the new header). Representative lines:

```
NotchController.cpp(57,17): error C2660: 'NotchController::pushClearLocked': function does not take 2 arguments
NotchController.cpp(107,23): error C2511: 'void NotchController::pushClearLocked(int,int)': overloaded member function not found in 'NotchController'
NotchController.cpp(135,22): error C2511: 'int NotchController::adoptPreset(const std::vector<PresetNotch,...>&)': overloaded member function not found in 'NotchController'
NotchController.cpp(151,17): error C2352: 'NotchController::setNotch': a call of a non-static member function requires an object
```

This is the RED the brief calls for (compile errors on the new/changed API), confirming the tests exercise the real signatures.

**GREEN** — after implementing the `.cpp` changes:

```
cmake --build build --config Release --target HandsFreeTests
```
→ builds clean (`HandsFreeTests.vcxproj -> ...\HandsFreeTests.exe`).

```
cd build && ctest -C Release --output-on-failure -R NotchController
```
→ `100% tests passed, 0 tests failed out of 58` — all 50 pre-existing `NotchController*` tests plus the 7 new ones (`EveryClearPathCarriesItsReason`, `SinkRunsOutsideTheModelMutexAndMayReenter`, `NoSinkMeansNoAccumulation`, `OutboxDropsAndCountsPastTheCap`, `StopFlushesPendingEventsEvenWhenTheThreadNeverRan`, `WideningResetsLaneOneDetectorState`, `AdoptPresetCountsLaneOneNotchesSkippedOnAMonoSlot`), no assertion changes to any existing test. `WideningResetsLaneOneDetectorState` passed against the unmodified 1e-3f bound — `Detector::reset()` genuinely zeroes `history_`, no loosening needed.

**Full tree** — `cmake --build build --config Release` builds `HandsFree`, `HandsFreeSnapshot`, and `HandsFreeTests` clean (only pre-existing, unrelated warnings: `LockFreeRingBuffer` padding, `NotchListPanel.cpp` float→int, one deprecated JUCE API in `MainComponent.cpp`). Confirms `MainComponent.cpp` needed no edit.

**Full suite gate**:
```
cd build && ctest -C Release
```
→ `100% tests passed, 0 tests failed out of 421`.

Note on the brief's arithmetic: it says "100% tests passed (416)" at focused-test time and "(414)" then "414 + 8 = 422" for the full gate. The brief lists 7 `TEST(...)` blocks in Step 1 (`EveryClearPathCarriesItsReason` covers all four clear-reason scenarios inside one test, not four separate `TEST` macros) — actual count added was 7, and the baseline before this task was 414, giving 421 total, which matches what ctest reports. Confirmed by `git diff --stat` and `grep -c '^+TEST' ` on the test file diff: 7 new `TEST` macros. This is a labeling discrepancy in the brief's test count, not a functional gap — every clear-reason scenario the brief asked for is asserted.

## Files changed

- `src/app/NotchController.h` — `ClearReason` enum, `SpectralContext`/`NotchEvent` structs, `EventSink`, `kMaxPendingEvents`, new public API, new private members/methods, `<functional>`/`<memory>` includes.
- `src/app/NotchController.cpp` — implementation of all of the above; every existing `pushClearLocked` call site updated to carry an explicit `ClearReason`; both partial-apply unwind sites now say `PartialApplyUnwind`.
- `tests/test_notchcontroller.cpp` — the brief's 7 tests appended verbatim after the stereo section.

`src/app/MainComponent.cpp` — NOT modified (compiled unchanged against the new default parameters, verified by the full-tree build).

## Self-review

- All six `ClearReason` values are exercised by name in `EveryClearPathCarriesItsReason` (Manual, VerdictFalse, ClearAll, AutoRelease, WidthChange, PartialApplyUnwind) — every value in the enum has a test asserting it reaches the sink.
- Cap + drop count: `OutboxDropsAndCountsPastTheCap` asserts both the capped queue size and the exact drop count (`200 - kMaxPendingEvents`).
- No-sink-no-queue: `NoSinkMeansNoAccumulation` asserts zero pending and zero dropped with no sink attached — confirms `pushEventLocked` returns before touching `eventOutbox_` when `eventSink_ == nullptr`.
- `stop()` flush: `StopFlushesPendingEventsEvenWhenTheThreadNeverRan` — thread never started, `stop()` still delivers the queued Set.
- Widen-reset: `WideningResetsLaneOneDetectorState` — passes at the un-loosened 1e-3f bound.
- Skipped count: `AdoptPresetCountsLaneOneNotchesSkippedOnAMonoSlot` — asserts both the returned adopted count and `*skippedOut`.
- Names match the brief exactly (`ClearReason`, `NotchEvent`, `EventSink`, `pushEventLocked`, `flushEventOutbox`, `setNotchImpl`, etc.); no renaming for "clarity".
- No over-building: `hasScore`/`confirmedLane`/`score`/`peakiness`/etc./`ctx` fields on `NotchEvent` are declared (required by the type, since Task 4 fills them and tests copy the struct) but never populated by this task — every `Set` event emitted here has `hasScore == false`, exactly as the brief specifies for Task 3's scope.
- Test output is pristine: no new warnings introduced, `ctest -C Release` full run shows `100% tests passed, 0 tests failed out of 421`, no skipped/disabled tests.
- Verified `PresetNotch::lane` defaults to `-1` in `src/app/PresetManager.h:103` before relying on `onePresetNotch()` leaving it unset, per the brief's instruction.
- Verified `Detector::reset()` (`src/dsp/Detector.cpp:42`) zeroes `history_` (the whole analysis window) — matches what the widen-reset test needs; no threshold loosening was required.

## Concerns

None. Every existing `NotchController*` test passed with no assertion changes (58/58 focused, 421/421 full suite). Expected level change: 0 dB — this task is pure plumbing on the message/detector-thread side; the audio thread and DSP path are untouched.

## Fix round 1 report

Reviewer returned NEEDS FIXES on the initial round-1 controller work (commit `4cad420`). All six findings addressed; no other lines touched.

**Important 1 — widen-reset removed (`src/app/NotchController.cpp:75-99`, `setWidth`).** Deleted the entire `if (newWidth > width_)` branch that called `Detector::reset()` and zeroed `persistence`/`previousBlockNowMs` for lanes re-entering scope, and deleted its test `NotchControllerSlotAware.WideningResetsLaneOneDetectorState` (`tests/test_notchcontroller.cpp`, was at line ~1419). Replaced with a comment at the same spot in `setWidth` explaining why: resetting only the `Detector` leaves `CandidateScorer`'s rise history and baseline EMA holding pre-mono audio, and once the zeroed frames age past `0.45 x riseReferenceMs` they become the reference and saturate the rise/novelty axes (`max(ref, 1e-12)` floor) for ~100 ms — a MORE permissive detection window than shipped behaviour. Lane D may not change detection behaviour (owner constraint: 0 dB, no audio/detection-path change). The comment records this as an OPEN OWNER DECISION for lane S, with the pointer "see SDD ledger 2026-09-05-data-loop, Task 3 ruling". The narrowing branch (`ClearReason::WidthChange`) is untouched.

**Important 2 — event-sink thread comment fixed (`src/app/NotchController.h:162-168`).** Was: "delivered to the sink from the DETECTOR thread by flushEventOutbox()". Now: "delivered to the sink from the detector thread by flushEventOutbox(), or from the caller of stop() after the join; never concurrently. Never from the audio thread." — matches that `stop()` also calls `flushEventOutbox()` synchronously on the caller's thread.

**Minor 3 — `setEventSink` lifetime contract (`src/app/NotchController.h:197-202`).** Added a comment directly above the declaration: the sink must outlive the controller's last `stop()` (the destructor calls `stop(2000)` and may invoke the sink from it); an owner capturing `this` in the sink must call `stop()` on the controller before its own members die, or call `setEventSink(nullptr)` after that join.

**Minor 4 — `SinkRunsOutsideTheModelMutexAndMayReenter` fixed (`tests/test_notchcontroller.cpp`, ~line 1355).** `mutexWasFree` was overwritten on each of the three sink invocations, so only the last call's observation was ever asserted. Changed to `bool mutexWasFree = true;` and `mutexWasFree = mutexWasFree && h.controller.modelMutexIsFreeForTest();` inside the sink, so the assertion now covers all three observations (AND). Comment added explaining the round-1 bug.

**Minor 5 — `stop()` drains the outbox until empty (`src/app/NotchController.cpp:39-48`, `.h:118-121` and `:311-315`).** `flushEventOutbox()` now returns `bool` (true iff it had something to deliver — including "attempted delivery with no sink"), and `stop()` loops `for (int i = 0; i < 8 && flushEventOutbox(); ++i) {}` instead of a single call, so a sink that re-enters `clearNotch()`/`setNotch()` while `stop()` flushes gets its follow-on event drained too, bounded at 8 passes so a sink that never stops re-entering cannot hang shutdown. `runOnce()`'s call site (`flushOutbox(); flushEventOutbox();`) is unaffected — it already discards the return value, which is fine there (`run()`'s poll loop calls it again 5 ms later regardless). Added test `NotchControllerEvents.StopDrainsEventsQueuedByAReentrantSinkDuringItsOwnFlush` (`tests/test_notchcontroller.cpp`, appended after `StopFlushesPendingEventsEvenWhenTheThreadNeverRan`): a never-started controller gets one `setNotch`, the sink re-enters `clearNotch()` on the Set event, `stop(1000)` is called, and the test asserts the re-entrant Clear was delivered too. This test goes red if `stop()` reverts to a single `flushEventOutbox()` call.

**Minor 6 — `adoptPreset` comment narrowed (`src/app/NotchController.h:157-160`).** Was: "the number of notches this slot could not take at all". Now: "the count of notches naming a lane this slot does not have — today only lane 1 on a mono slot. A notch that fails validation on every lane it targets is neither adopted nor counted here." No logic change — `firstLane >= width_` is still the only path that increments `skipped`.

### Build and test evidence

```
cmake --build build --config Release
```
→ builds `HandsFree`, `HandsFreeSnapshot`, `PeakinessSweep`, `HandsFreeTests` clean. Only pre-existing, unrelated warnings (`LockFreeRingBuffer` alignment padding, `NotchListPanel.cpp` float->int narrowing, one deprecated JUCE API in `MainComponent.cpp`).

```
cd build && ctest -C Release --output-on-failure -R NotchController
```
→ `100% tests passed, 0 tests failed out of 58` (one test removed — `WideningResetsLaneOneDetectorState` — one added — `StopDrainsEventsQueuedByAReentrantSinkDuringItsOwnFlush` — net count unchanged at 58).

```
cd build && ctest -C Release
```
→ `100% tests passed, 0 tests failed out of 421` (net unchanged from before this fix round: -1 deleted test, +1 new test).

### Concerns

None outstanding from this fix round. The widen-reset removal is a behaviour change relative to the initial Task 3 commit (a widened lane's Detector/persistence/clock are no longer reset), but it restores the pre-Task-3 shipped behaviour exactly — this fix round is strictly a revert-plus-documentation on that path, not a new behaviour. Expected level change: 0 dB, same as the original Task 3 report — nothing on the audio thread or DSP path was touched in this round either.
