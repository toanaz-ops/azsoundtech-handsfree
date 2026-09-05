# Task 5 report: `NotchListPanel` GOOD / FALSE buttons

## What was done

Added a VERDICT column to `NotchListPanel` (the ACTIVE NOTCHES table): two
real child `juce::TextButton`s per notch identity (GOOD / FALSE), keyed by
the same `identityKey()` the `sightings_` ledger already uses so a click
cannot land on a button a rebuild just deleted. The panel still issues no
command — it reports through the new `onVerdict` callback only; wiring that
callback to the logger and to `clearNotch(..., VerdictFalse)` is Task 6, not
this one.

Implemented exactly per `.superpowers/sdd/2026-09-05-data-loop/task-5-brief.md`:

- `enum class Verdict : std::uint8_t { None, Good, False };`
- `std::function<void (int slot, int lane, int index, float hz, bool good, double ageMs)> onVerdict;`
- `kColVerdictW = 92.0f`, `kVerdictButtonW = 40`, `kVerdictButtonH = 20`, `kVerdictGap = 4`
- Test accessors: `goodButtonForTest`, `falseButtonForTest`, `verdictForTest`, `displayedSlotForTest`
- `RowText` gained `key`, `verdict`, `verdictText`
- New private: `RowButtons` (owns the two buttons + state), `buttons_` map keyed
  by identity, `displayedSlot_`, `ensureButtonsFor`, `layoutButtons`, `reportVerdict`
- `refreshFromSnapshot()`: loop 1 now calls `ensureButtonsFor` per seen identity;
  loop 2 sets `row.key`/`row.verdict`/`row.verdictText` from `buttons_`; loop 3
  erases `buttons_[key]` alongside the expired sighting; `layoutButtons()` runs
  before `repaint()`.
- `resized()` now always ends with `layoutButtons()` (the `slotTabs_` block is
  gated by its own `if`, not an early return).
- `paint()` draws a `VERDICT` header at `x5 = x4 + statusW`, and the row's
  `verdictText` in `accent` (GOOD) or `danger` (FALSE) once non-empty.
- `statusWidthFor()` now subtracts `kColVerdictW` too.
- `reportVerdict` is idempotent (a second click on an already-verdicted row
  is a no-op) and relies on `rows_[i] <-> snapshot_.notches[i]` order —
  documented in a comment at the call site, as the brief specifies.

## TDD evidence

**RED** (compile errors on the new API, as expected):
```
error C2039: 'goodButtonForTest': is not a member of 'gui::NotchListPanel'
error C2039: 'verdictForTest': is not a member of 'gui::NotchListPanel'
error C2039: 'kColVerdictW': is not a member of 'gui::NotchListPanel'
... (Verdict, kVerdictButtonW, kVerdictGap, falseButtonForTest all likewise undeclared)
```

**GREEN** after implementation — focused run:
```
$ ctest -C Release --output-on-failure -R NotchListPanel
100% tests passed, 0 tests failed out of 16
```
(13 pre-existing NotchListPanel tests + the 3 new `NotchListPanelVerdict` tests.)

### One deviation from the brief's literal test text (with cause)

The brief's Step 1 code for `ButtonsPersistAcrossRefreshesAndDieWithTheirIdentity`
(and `ClickingFalseOrGoodReportsTheVerdictAndTheRowShowsIt`) constructs the panel as:
```cpp
FakeClock clock;
gui::NotchListPanel panel (fed.controller, clock);
```
`ClockFn` is `std::function<double()>`; passing `clock` (an lvalue) by value
copies the `FakeClock` struct into the `std::function`'s storage at
construction time (nowMs = 0.0). Every later `clock.nowMs += ...;` in the
test mutates the *outer* copy only — the panel's captured clock stays frozen
at 0 forever. This is exactly the trap the same test file already documents
on `AgeIsComputedFromFirstSightingNotWallTime` ("Capture BY REFERENCE: ClockFn
copies its callable, so passing `clock` by value would freeze the panel on a
snapshot of nowMs").

I proved this empirically: instrumented `refreshFromSnapshot()` with `fprintf`
tracing and reran `ButtonsPersistAcrossRefreshesAndDieWithTheirIdentity` — every
refresh reported `now=0.000000`, so the identity never crossed
`kTrackingTimeoutMs` and the test's final `EXPECT_EQ (getNumChildComponents(), 2)`
failed (stayed at 4). This is a defect in the literal brief text, not in the
implementation — with the by-value construction, no correct implementation of
`ensureButtonsFor`/expiry can pass that assertion, since the panel's own clock
never advances.

Fix: changed both constructions to `gui::NotchListPanel panel (fed.controller,
[&clock] { return clock.nowMs; });`, matching the file's own established,
documented pattern. Removed the debug `fprintf` instrumentation before the
final build. This is the only place the tests deviate from the brief's literal
text; every constant, name, and production-code shape matches the brief exactly.

## What I saw in `shots/console-live.png`

Rendered with `build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast`,
then read back (full frame, then a 2x crop of the ACTIVE NOTCHES panel).
Confirmed against the brief's Step 6 checklist:

- **VERDICT header**: reads in full, "VERDICT", right-aligned over the button
  pair, not truncated or ellipsised.
- **Both buttons on every row**: all three rows (01 L 247 Hz, 02 L 1.9 kHz,
  03 R 1.2 kHz) show `GOOD` and `FALSE` fully spelled out, side by side, in
  the ghost-chip mono font — no clipping.
- **HELD**: still shows its age text (`0s` on all three rows in this fresh
  snapshot) immediately to the left of the button pair — the new column did
  not push HELD into truncation.
- No other column caption was ellipsised (# / LANE / FREQ / DEPTH / Q / HELD
  all read cleanly at 1440 px width).

No fixes were needed before committing — the layout matched on the first render.

## Full suite

```
$ ctest -C Release
100% tests passed, 0 tests failed out of 425
```
(422 pre-existing + 3 new `NotchListPanelVerdict` tests = 425, as predicted.)

Full build (`cmake --build build --config Release`, all targets including
`HandsFree`, `HandsFreeSnapshot`, `HandsFreeTests`) also succeeded; the only
new warnings on the changed lines are `C4244` float-to-int narrowing in the
VERDICT-text `g.drawText` call, which follows the exact same pre-existing
narrowing pattern every other `g.drawText` call in this `paint()` already
uses (float column-origin constants passed to the `int`-overload) — not a
regression, just the file's established style.

## Files

- `src/gui/NotchListPanel.h`
- `src/gui/NotchListPanel.cpp`
- `tests/test_notchlistpanel.cpp`

## Self-review

- Verified `reportVerdict`'s row/snapshot correspondence comment is accurate:
  `rows_` is `clear()`d and rebuilt in snapshot order every
  `refreshFromSnapshot()`, so `rows_[i]` and `snapshot_.notches[i]` always
  refer to the same notch at the moment `reportVerdict` runs (buttons are
  never rebuilt between a click and the row lookup, since the click handler
  reads the CURRENT `rows_`/`snapshot_`, not a stale copy).
- Verified `ensureButtonsFor` is only called from loop 1, before `rows_` is
  rebuilt, so a freshly-appearing identity always has buttons before the row
  that references it is constructed in loop 2.
- Verified `buttons_.erase` happens in lockstep with `sightings_.erase` in
  loop 3 (same key, same iteration) — no identity can have a sighting without
  buttons or buttons without a sighting.
- Verified `layoutButtons()` hides every button first (so a row that gains a
  verdict, or a row that scrolled past the panel's bottom edge, never leaves
  a stale visible button behind), then shows only buttons for in-range,
  verdict-`None` rows.
- Verified `resized()` still lays out `slotTabs_` correctly when non-null,
  and now always calls `layoutButtons()` afterward regardless.
- Confirmed `kColVerdictW` (92) comfortably exceeds
  `2*kVerdictButtonW + kVerdictGap + 4 = 88`, and measured against the real
  faces via `VerdictColumnFitsItsButtonsAndCaption` — no font shrink was
  needed; the brief's constants fit as given.
- No limiter, clamp, NaN/denormal check, or audio-path code was touched —
  this is a pure GUI/display change; no DSP behavior, gain, or filter
  coefficient changed.

## Concerns

- The `FakeClock`-by-value deviation (above) is the only place the shipped
  test text differs from the brief's literal listing. I judged fixing it was
  required (the brief's literal test cannot pass under any correct
  implementation) rather than a NEEDS_CONTEXT stop, since the intended fix is
  unambiguous and matches an existing, documented convention in the same
  file. Flagging it explicitly here per the reporting instructions.
- `.superpowers/sdd/2026-09-05-data-loop/progress.md` shows as modified in
  `git status` but was not touched by this task — left alone, not staged.
