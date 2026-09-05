# Task 4 report: `notch_set` with score and spectral context

## What

Detector-origin `Set` events now carry `hasScore == true`, the full
`CandidateScorer::ScoreBreakdown` axes, and a `shared_ptr<const SpectralContext>`
holding the scored frame (`ctx->now`), the rise-reference frame the scorer
compared against (`ctx->ref`, when the history had one old enough), and the
other lane's frame from the same drain iteration (`ctx->other`, when present).

Changes, exactly per the brief:

- `src/app/NotchController.h`
  - Added the private `PlacementContext` struct (verbatim from the brief:
    `now`, `other`, `breakdown`, `finalScore`, `asymmetry`, `persistNeeded`,
    `thr`, `sampleRate`).
  - `placeConfirmed` signature gained the `const PlacementContext& pc` fourth
    parameter.
- `src/app/NotchController.cpp`
  - `processSpectrumForDetection`'s candidate loop: replaced
    `la.scorer.scoreCandidate(...)` + `score *=` with
    `la.scorer.scoreCandidateDetailed(...)` and
    `const float score = breakdown.score * asym;` -- one multiplication,
    `breakdown.score` first, same two operands in the same order as before.
    Score is arithmetically identical to the old path (verified: `asym` is
    computed by the exact same `asymmetryMultiplier` call, same arguments,
    same order). Expected level change: 0 dB.
  - At the confirm site, builds a `PlacementContext` from the block's
    magnitudes, the other lane's magnitudes, the breakdown, `finalScore`,
    `asym`, `requiredBlocks`, `la.analyzer.getThreshold()`, and
    `block.sampleRate`, then calls `placeConfirmed(lane, cand, linkedNow, pc)`.
  - `placeConfirmed`: before the `setNotch` fan-out loop, builds one
    `shared_ptr<SpectralContext>` (`ctx->binHz = pc.sampleRate /
    Detector::kFftSize`; copies `pc.now` into `ctx->now`; copies
    `pc.breakdown.refFrame` into `ctx->ref` and sets `hasRef`/`refAgeMs` when
    `refFrame != nullptr`; copies `pc.other` into `ctx->other` and sets
    `hasOther` when `pc.other != nullptr`), and one `NotchEvent scored`
    template with `hasScore = true`, `confirmedLane = lane`, the five score
    axes, `asymmetry`, `persistNeeded`, `thr`, and `ctx`. The fan-out loop was
    changed from `setNotch(...)` (which always passes `scored == nullptr`) to
    `setNotchImpl(l, index, cand.frequencyHz, q, depthDb, origin, &scored)` so
    every lane of a LINKED placement shares the same `ctx` allocation --
    exactly one `make_shared` per placed notch, on the detector thread (plan
    A-4), never one per lane.
  - Comment added above the allocation explaining why `pc.breakdown.refFrame`
    (pointing into the scorer's `history_`) is safe to copy here: it is only
    invalidated by `la.scorer.commitBlock()`, which runs in
    `processSpectrumForDetection` AFTER the whole candidate loop -- i.e.
    strictly after this `placeConfirmed()` call returns.
  - `#include <algorithm>` was already present (needed for `std::copy_n`); no
    include change required.
- `tests/test_notchcontroller.cpp`: appended the brief's Step-1 test verbatim
  as `NotchControllerEvents.DetectorPlacementCarriesTheScoredFrameAndTheScorersReference`.

### `warmThenDrive` helper

The brief asked me to check whether `warmThenDrive(h, quietL, toneR)` already
existed and add it if not. It already exists, at
`tests/test_notchcontroller.cpp:944` (anonymous namespace, right above
`IndepHowlOnRightOnlyCutsRightOnly`): warms both lanes with `NoiseSource` for
`kWarmupBlocks`, then pumps the given `left`/`right` sources for up to 40
blocks (breaking early once a command lands), pumps 6 more blocks so a lagging
second lane gets its chance, and returns `drain(h.commands)`. This is exactly
the semantics the brief described, under the exact name, so no new helper was
added.

## TDD evidence

**RED** (focused build + run, before the implementation edit):

```
$ cmake --build build --config Release   # compiled clean
$ cd build && ctest -C Release --output-on-failure -R NotchController
...
59/59 Test #401: NotchControllerEvents.DetectorPlacementCarriesTheScoredFrameAndTheScorersReference ...***Failed
...
tests/test_notchcontroller.cpp(1464): error: Expected: (set) != (nullptr), actual: NULL vs (nullptr)
no scored Set event reached the sink
[  FAILED  ] NotchControllerEvents.DetectorPlacementCarriesTheScoredFrameAndTheScorersReference
98% tests passed, 1 tests failed out of 59
```

Failed at exactly the assert the brief predicted (`ASSERT_NE (set, nullptr)`),
nowhere else -- confirming the test compiled cleanly against Task 3's fields
and was red only for the missing production wiring.

**GREEN** (after implementing Steps 3-4):

```
$ cmake --build build --config Release   # clean, only pre-existing warnings (C4324 alignment padding, C4244 float->int in NotchListPanel.cpp, a JUCE deprecation) -- none touch the changed files' logic
$ cd build && ctest -C Release --output-on-failure -R NotchController
...
59/59 Test #401: NotchControllerEvents.DetectorPlacementCarriesTheScoredFrameAndTheScorersReference ...   Passed    0.03 sec
100% tests passed, 0 tests failed out of 59
```

**Full suite** (Step 5 gate):

```
$ cd build && ctest -C Release
...
100% tests passed, 0 tests failed out of 422
```

422 tests, as predicted (was 421 before this task's one new test).

## Files

- `src/app/NotchController.h`
- `src/app/NotchController.cpp`
- `tests/test_notchcontroller.cpp`

## Commit

`ae3cfa3` -- `feat(controller): detector placements log score axes and the scored/reference frames`

Staged explicitly (`git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp`);
`git status` before commit showed no other files staged.

## Self-review

- **Completeness**: every artifact in the brief's Interfaces/Produces section
  is wired -- `hasScore`, `ctx != nullptr`, `ctx->now`, `ctx->ref` (when
  `refFrame` non-null), `ctx->other` (when the other lane produced a block
  this iteration). `PlacementContext` and the `placeConfirmed` signature match
  the brief's code block field-for-field and name-for-name.
- **Names**: `PlacementContext`, `pc`, `ctx`, `scored`, `breakdown`, `asym` all
  match the brief's exact identifiers. No renaming.
- **No over-building**: nothing beyond the brief was added -- no new public
  API, no extra fields on `NotchEvent`/`SpectralContext` (those were Task 3's),
  no changes to `setWidth` (left untouched per the task instructions), no
  helper added since `warmThenDrive` already existed under the exact name.
- **Test has teeth**: the appended test checks five independent things that
  the implementation could get individually wrong -- `hasScore`/`origin`,
  the axis product equalling the recorded score (would catch e.g. `asymmetry`
  not being folded into `score` or a wrong axis assignment), `ctx->now`'s
  argmax bin matching the placed frequency (would catch passing the wrong
  frame, e.g. `pc.other` instead of `pc.now`), `ctx->ref`'s age bounds and the
  rise ratio it implies (would catch `refFrame`/`refAgeMs` not being copied,
  or copied from the wrong lane's history), and `ctx->other` being present and
  quiet at the howl bin (would catch `pc.other` not being threaded through,
  or `hasOther` not gated on non-null). It was run RED before GREEN and failed
  at the predicted line.
- **Output pristine**: `ctest -C Release` for the focused run and the full
  suite both show `100% tests passed` with no skipped/disabled tests; no new
  compiler warnings were introduced in `NotchController.cpp` (`cl` emitted
  none for that file in either build).
- **Live-sound DSP claim**: the score computation is unchanged in value --
  `scoreCandidateDetailed(...).score` is the exact same arithmetic
  `scoreCandidate(...)` used to compute internally (per `CandidateScorer.h`'s
  own comment: "`scoreCandidate()` returns `.score` of this and nothing else
  -- one arithmetic path, never two"), and `score = breakdown.score * asym`
  reproduces `score *= asymmetryMultiplier(...)` with the same two operands
  in the same order. Confirmed at the code level; did not additionally
  instrument a numeric before/after diff since the scorer guarantees a single
  arithmetic path shared by both entry points.

## Concerns

None outstanding. The one thing worth flagging for whoever builds Task 6 (the
session log): `ctx->other` is populated straight from `otherLaneMagnitudes`,
which `runOnce()`'s per-lane drain invariant says is "at most one hop apart"
from `block` -- not guaranteed to be from literally the same instant. That
is inherited behavior (Task 3's field comment already says "the other lane's
frame of the same drain iteration"), not something this task changed or needs
to fix.
