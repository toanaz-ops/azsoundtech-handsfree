# Task 9 report — the lane M GUI

**Status:** DONE. Commit `f7a2272` on `feat/lane-m-active-soundcheck` (parent `f8437df`).
**Model actually used:** Claude Opus 5 (1M context), `claude-opus-5[1m]`.
**Expected level change: 0 dB.** No audio path, no filter coefficient, no gain stage,
no buffer handling touched. `DỪNG` is a safety control, not a convenience — it is the
only thing in this change that an operator will reach for while sound is going into a PA.

---

## 1. Verification — commands and their output

Configure + build (header change, so a full reconfigure):

```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```
Clean: zero `error C` / `error LNK` / `error MSB` lines. `HandsFreeSnapshot.exe`,
`HandsFree.exe` and `HandsFreeTests.exe` all relinked.

Focused:

```
cd build && ctest -C Release -R "ModeRail|SpectrumView|GuiWiring|SoundcheckPanel" --output-on-failure
  100% tests passed, 0 tests failed out of 62
```

Full gate, run against **exactly the committed tree** (I re-ran it after restoring
from HEAD following the baseline-render experiment in §4):

```
cd build && ctest -C Release
  100% tests passed, 0 tests failed out of 683
  Total Test time (real) =  60.94 sec
```

**683, not the brief's 633 and not the parent's 676.** Baseline at `f8437df` was 668;
this task adds **15** (ModeRail +5, SoundcheckPanel +6, SpectrumView +4), not 8. The
brief's 625 baseline was stale by three tasks.

Theme discipline:

```
./build/tests/Release/HandsFreeTests.exe --gtest_filter="AzTheme*"
  [  PASSED  ] 15 tests.        (includes NoHexColourLiteralsOutsideTheThemeFolder)
```

### Mutation check — the label tests CAN fail

Rebuilt `measureButton` and `applyButton` with the JUCE 9 two-argument form
`TextButton(name, tooltip)`, rebuilt, ran:

```
1/3 Test #233: SoundcheckPanel.ModesShowTheRightControlsWithRealLabels ...***Failed
    Which is: apply
    Which is: ÁP DỤNG
2/3 Test #246: ModeRail.MeasureButtonHasItsLabel .........................***Failed
    Which is: measure
    Which is: ĐO
33% tests passed, 2 tests failed out of 3
```

Both restored (`cmp` against the pre-mutation copies) and rebuilt before committing.

**A correction to `memory/juce9-api-traps-2026-08-25.md` falls out of this.** In *this*
JUCE build the two-argument constructor does **not** render an empty face — the first
argument still became the button text (`"measure"`, `"apply"`). So the trap as recorded
("the button renders with NO TEXT AT ALL") does not reproduce here. What the two-argument
form *does* do is silently take a second string that is not a tooltip, and the label that
ships is then whatever the first argument was. The tests assert the **exact expected
label** rather than merely non-emptiness, which catches both shapes of the bug; the
`isEmpty()` assertions are kept as a second, narrower net. Worth an amendment to that
memory note — I have not edited it, since it is a shared index and another lane may be
in it (global rule 7).

---

## 2. The pictures — every PNG, and what I SAW in each

All rendered with `build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast`
from the committed tree; md5s below are of the files as they stand.

| file | md5 |
|---|---|
| console-idle.png | `ce7158504a9a6500371f9c58683cc062` |
| console-live.png | `985e6ca596ca4744a4aadee047649172` |
| console-preset-music.png | `61a97306380387a7a03706c4399438b5` |
| console-soundcheck-running.png | `b76de4410171217db2dc432f45ba8505` |
| console-soundcheck-results.png | `5111626bc2ea5206b1d70e3fe807f315` |

**`D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-m-soundcheck-0915\shots\console-soundcheck-results.png`**
— strip below the plot, not over it: the frequency axis (80…15k) and the five red
marked-bin ticks along the plot floor at 247 / 660 / 1.24k / 1.92k / 3.4k are all
visible; the white margin curve is clearly separable from the amber trace and peaks
exactly where the trace does; the low-confidence band right of 6 kHz is a visibly
lighter panel carrying a legible `độ tin cậy thấp`; the headline `tìm thấy 5 điểm dễ hú`
is amber and one size up from the three sentences under it
(`1 điểm còn vượt sau khi cắt sâu nhất — chỉnh gain, hạ trần, hoặc đổi vị trí mic`,
`1 kênh không đo được`, `1 kênh sai định tuyến`); `ÁP DỤNG` and `BỎ` both fit their
buttons with room; no mojibake anywhere; the three notch stems (01, 02, 03R) still draw
over the overlay, so nothing the detector placed is hidden by a curve describing a
measurement.

**`…\shots\console-soundcheck-running.png`**
— all four rail switches and `ĐO` visibly greyed (F10 lock), `ĐANG ĐO · KÊNH 2/4` as a
silkscreen caption over `48 s` in large amber mono, `DỪNG` as a red outlined cell on the
right; the analyser keeps its full axis; the accent stripe down the strip's left edge is
sodium (it is `dim` in Results, which is how the two states differ at a glance).

**`…\shots\console-live.png`**
— unchanged from `f8437df` except the new `ĐO` cell (proved by pixel diff, §4); the
legend `ĐO` and the hint `phát tín hiệu` both render, untruncated, in a cell narrower
than a mode switch; the analyser, the notch table with its GOOD/FALSE verdicts, the rig
column and the staged CRITICAL ring-risk chip are pixel-identical to the baseline.

**`…\shots\console-idle.png`**
— `ĐO` in its **enabled** state (compare the running shot: the disabled alpha is a real,
visible difference), panel hidden, analyser at full height, `WAITING FOR SIGNAL` and
`NOTHING RINGING` empty states intact.

**`…\shots\console-preset-music.png`**
— the off-list-ceiling shot, unchanged except the `ĐO` cell. Still renders Q 25 / −10 dB
as text in the combos.

### Three defects the render found that every assertion passed through

This is the reason for the standing instruction, and it earned its keep again:

1. **The strip buried the frequency axis and the overlay's own marked-bin rake.**
   Round 1 laid `SoundcheckPanel` *over* the bottom of the analyser. The panel was
   inside its own bounds, the buttons did not overlap, the summary did not run under
   them — every test passed. The picture showed the entire x-axis and all five red
   ticks gone. Fixed: the strip now **takes a band off the bottom** of the analyser
   (`MainComponent::resized()`), with a `kMinSpectrumHeight` fallback to overlaying
   on a window too short for both.
2. **The low-confidence band was invisible.** It was filled with `shade` — which is
   *darker* than `background`, the plot's own ground. At 0.20 and again at 0.42 the
   render showed nothing at all. A band nobody can see is a caveat nobody reads, and
   the operator is then looking at a measurement that stops dead at 6 kHz with no
   explanation. Fixed: `border` at 0.22, which **lightens** — the only direction that
   can work on a near-black ground.
3. **The margin curve was drawn in `peak`**, which already means "the peak-hold ceiling
   of this same trace" on this same plot. Changed to `text`, the plain silkscreen
   foreground, which claims no semantic. (It is deliberately not `cooling`/`settled`
   either — those are two stops of the notch age ramp, and bolting a second meaning
   onto them is exactly how `ice` would degrade into decoration.)

---

## 3. What was built

- **`src/gui/ModeRail.{h,cpp}`** — `measureButton` (one-argument ctor, explicit UTF-8
  bytes), `onMeasure`, `setMeasureEnabled`, `setModeControlsEnabled`. Momentary:
  **not** in the radio group and not latching, so it can never silently un-light the
  mode the engine is actually in. Its cell width is measured in the constructor with
  the real legend font *and* the real hint font, and the countdown block is now
  positioned off `measureButton` rather than `bypassButton`.
- **`src/gui/SoundcheckPanel.{h,cpp}`** — new. `Mode{Hidden,Running,Results}`,
  `setProgress`, `setResults`/`setResultsSummary`, `onStop/onApply/onDismiss`,
  `onModeChanged`, `keyPressed`. Holds no controller; formats and draws only.
- **`src/gui/SpectrumView.{h,cpp}`** — `setSoundcheckOverlay` / `clearSoundcheckOverlay`
  / `hasSoundcheckOverlay`, `kLowConfidenceAboveHz`, `kMarginAtRiskDb`/`kMarginSafeDb`,
  `kSoundcheckOverlayReserveFloats`, and the two test accessors. One reused
  `juce::Path`, preallocated in the ctor.
- **`src/app/MainComponent.{h,cpp}`** — owns and lays out the panel, and two test
  accessors. **No controller wiring** (Task 10).
- **`tools/snapshot.cpp`** — two more `shoot()` blocks, both STAGED and saying so on
  stdout, in the same linear shape as the other three.

---

## 4. `console-live.png` / `console-idle.png` — the unchanged-except-`ĐO` claim, proved

Not asserted, measured. After committing, I checked the eight source files out at
`f8437df` into the same build directory (so the JUCE objects stayed cached), rebuilt
`HandsFreeSnapshot`, rendered the three pre-existing scenes to a baseline directory,
then restored HEAD and rebuilt. A byte-level RGBA comparison of the decoded PNGs:

```
console-idle.png             differing px:   5387  bbox x[540..699] y[50..111]
console-live.png             differing px:   5387  bbox x[540..699] y[50..111]
console-preset-music.png     differing px:   5387  bbox x[540..699] y[50..111]
```

The transport band is y 38…124 and `BYPASS` ends at x ≈ 532. So **every** changed pixel
in all three shots lies inside the new `ĐO` cell plus the countdown block it pushed
right — identical bounding box in all three, and nothing below the transport moved. The
analyser, the notch table, the rig column and the routing table are bit-identical.

(Restored afterwards: `git status` clean for `src/`, `tools/`, `CMakeLists.txt`; HEAD
`f7a2272`; full suite re-run green at 683 on the restored tree; the five PNGs re-rendered
to identical md5s.)

---

## 5. Divergences — brief vs. code, and where I went past the brief

Code won everywhere the two disagreed. Every difference, listed:

1. **`LoopGainEstimator::kTrustedHighHz` is a `double`, not a float.** The brief writes
   `static constexpr float kLowConfidenceAboveHz = LoopGainEstimator::kTrustedHighHz;`.
   Written with an explicit cast. Value 6000.0, exactly representable, so no precision
   question — but the implicit narrowing was worth not shipping.
2. **The dimmed band is 6 kHz → the top of the displayed range, not "6–10 kHz".** The
   parent's task text said 6–10 kHz; the brief's Step 5 says `[kLowConfidenceAboveHz,
   kMaxHz]`. Followed the brief. There is nothing special about 10 kHz — the estimator
   distrusts everything above 6 kHz equally.
3. **The overlay is drawn in the empty-state branch too**, not only "after the spectrum
   polyline and before the notch stems" as Step 5 says. §4.1 is the reason: the taps are
   suspended for the whole run, so a soundcheck started on a console that has not
   published a frame yet has `snapshot_.sequence == 0` for all 72 s, and the brief's
   single insertion point sits *after* the early return. It would have shown an empty
   grid for the entire measurement. `SpectrumView.SoundcheckOverlayDrawsEvenWithNoPublishedSpectrum`
   is the test; it also un-vacuums the brief's own no-allocation test, which builds a
   bare controller (sequence 0) and would otherwise have asserted `0 == 0` a hundred times.
4. **`OutputResult::marginDb` is `std::array<float, LoopGainEstimator::kNumBins>`**, while
   the brief's SpectrumView tests size their vectors with `Detector::kNumBins`. Not a
   divergence in practice — `LoopGainEstimator.h:60` is literally
   `static constexpr int kNumBins = Detector::kNumBins;` — but the two names are not
   interchangeable by contract, only by current definition. Kept the brief's spelling in
   the tests, used `Detector::kNumBins` for the view's own storage.
5. **`SoundcheckPanel::Model` carries a fifth field, `cannotPropose`**, and `setResults`
   is added beside the brief's four-argument `setResultsSummary` (which is kept and
   forwards). The parent's contract note requires `ceilingMissing`/`ladderMissing` to be
   shown as an **ERROR, never as "room clean"**, and the four-int signature cannot
   express it. `SoundcheckPanel.AMissingCeilingIsAnErrorAndNeverReadsAsACleanRoom`
   pins it. Related: with `hotSpots == 0` the panel prints `không tìm thấy điểm dễ hú nào`
   **only** when there is no error *and* nothing was unmeasured.
6. **`ModeRail::setModeControlsEnabled` is not in the brief's "Produces" list.** The
   brief's prose requires the F10 lock on SOUNDCHECK / AUTO / BYPASS / CLEAR ALL; those
   four are the rail's own controls, so the lock belongs on the rail. Preset and
   slot-control locking is MainComponent's and stays Task 10.
7. **`SoundcheckPanel` is a child of `MainComponent`, with `onModeChanged` and two test
   accessors** (`getSoundcheckPanelForTest`, `getModeRailForTest`). The snapshot scenes
   cannot exist otherwise — there would be nothing to render. The panel is still driven
   entirely by plain data; Task 10 only has to feed it and connect the three callbacks.
8. **The brief's claim that "there is no 'sweep the room' placeholder anywhere in this
   repo" is wrong.** `src/gui/ModeRail.cpp:40` sets exactly that string as
   `soundcheckButton`'s hint. It is a hint, not a button, so the brief's *conclusion*
   (that `ĐO` needed building from scratch) holds — but the statement itself does not,
   and the hint now sits directly beside `ĐO`'s own `phát tín hiệu`.
9. **Two new `shoot()` blocks, not one.** Brief Step 6 says a fourth; the parent asked
   for a running scene as well. There are now five scenes.
10. **`setResultsSummary`'s `hotSpots` is fed `candidateCount`, not `markedCount`**, in
    the snapshot scene. They differ in general (a marked bin need not yield a
    proposal); "hot spots found" is what would be *placed*, so the candidate count is
    the honest number. Task 10 should feed it the same way.

---

## 6. Self-review

- **Theme tokens, no raw colours** — `AzTheme.NoHexColourLiteralsOutsideTheThemeFolder`
  passes; `grep` for `0x......`/`#......` in the two new files returns nothing.
- **No layout in `paint()`** — `SoundcheckPanel` computes `captionArea_`, `summaryArea_`
  and `accentBar_` in `resized()`; `paint()` only reads them. `SpectrumView`'s overlay
  uses `xForHz`/plot arithmetic, which is what the rest of that `paint()` already does.
- **No allocation in `paint()` for the overlay** — one member `juce::Path`,
  `preallocateSpace(kSoundcheckOverlayReserveFloats)` in the ctor, `clear()` + rebuild
  per paint. `SpectrumView.SoundcheckOverlayPaintsWithoutAllocating` paints 101 times
  and asserts the element count is unchanged **and** that
  `elements * 3 <= kSoundcheckOverlayReserveFloats`, i.e. that it never grew past what
  the ctor reserved. Same honest caveat as `dashedStemPathElementCountForTest`:
  `juce::Path` exposes no capacity getter, so the iterator walk is a proxy, not a proof
  of no realloc.
- **Every label test can fail** — mutation-checked above, output pasted.
- **Vietnamese survives** — every string in `src/` and in the new test blocks is
  explicit UTF-8 bytes through `juce::String::fromUTF8`, following
  `src/gui/DeviceViewModel.cpp:13`. This build passes **no `/utf-8`** to MSVC and only
  `tests/test_spectrumview.cpp` carries a BOM, so a source literal would be decoded with
  the machine's active codepage. `src/gui/ModeRail.h`, `ModeRail.cpp`,
  `SoundcheckPanel.{h,cpp}`, `SpectrumView.cpp`, `tools/snapshot.cpp` and all three
  appended test blocks are **byte-for-byte ASCII** (checked programmatically). Where a
  hex escape is followed by a character that is itself a hex digit the literal is split
  (`"…\xa3" "c"`) — C++ hex escapes are greedy, and `"\xa3c"` would compile to the wrong
  bytes in silence. The rendered PNGs and the gtest failure output both confirm the
  bytes decode correctly.
- **Never `git stash`**, never `git add .`/`-A` — thirteen explicit paths, confirmed with
  `git diff --cached --name-only` before committing. `shots/` is untracked
  (`git ls-files shots` → 0) and was **not** committed.
  `.superpowers/sdd/.gitignore` did not exist; `rm -f` run anyway before staging.

---

## 7. Concerns for the next session

1. **`memory/juce9-api-traps-2026-08-25.md` needs an amendment** (§1). The recorded
   symptom — an empty button face — does not reproduce in this JUCE build. I did not
   edit the shared memory index; someone should, with the real symptom.
2. **`getRemainingMsInRun()` is not yet proven to drive the panel.** Task 9 formats a
   `double` it is handed; nothing yet checks that Task 10 hands it lane M's number
   rather than `NotchController::getSoundcheckRemainingMs()`. That substitution would
   compile, pass every test in this task, and show a frozen countdown on a live PA
   (F12). Task 10 needs a test that fails on it.
3. **Apply-outcome reporting is not built.** `SoundcheckApplyStats` — in particular
   `clearedPrevious > 0 && placed == 0`, the real "worse off" outcome that must not be
   shown as success — has no home in this panel. It happens *after* `ÁP DỤNG`, when the
   panel is already `Hidden`, so it belongs to Task 10/11 and probably wants a fourth
   Mode or the status badge. Flagged so it is not assumed done.
4. **The strip covers ~104 px of analyser while it is up.** At 920 px tall that is
   comfortable; on a short window the `kMinSpectrumHeight` fallback puts it back on top
   of the plot, and in that case the axis and the marked-bin rake are buried again. No
   better answer exists at that size, but an operator on a small laptop will see it.
5. **The `ĐO` hint is Vietnamese (`phát tín hiệu`) while the three mode hints beside it
   are English** (`sweep the room` / `catch and hold` / `filters out`). Deliberate —
   lane M is Vietnamese-facing throughout — but it is visibly mixed in the render and
   the owner may want the other three translated, or this one not.

---

# Task 9 — fix report, review round 1

**Commit `7ef8848`** on `feat/lane-m-active-soundcheck` (on top of `f7a2272`).
Model actually used: **Claude Opus 5 (1M context)**, `claude-opus-5[1m]`.
**Expected level change: 0 dB.** No audio path touched.

All 4 Important + 4 minor + 2 gaps applied. One of my own fixes broke the render
and the render caught it — written up in §3 below, because it is the second time
in this task that a green suite said nothing about whether the thing was legible.

## 1. Verification

```
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release          # clean: 0 error C / error LNK
cd build && ctest -C Release -R "ModeRail|SpectrumView|GuiWiring|SoundcheckPanel" --output-on-failure
  100% tests passed, 0 tests failed out of 68
cd build && ctest -C Release
  100% tests passed, 0 tests failed out of 689
```

683 → **689**: +6 (`ThePanelAsksForEnoughHeightToDrawEveryFaultSentence`,
`AnApplyThatClearedAndPlacedNothingIsReportedAsWorseOff`,
`ASaturatedBinIsNeverAccompaniedByTheCleanRoomLine`,
`TheSoundcheckStripTakesABandOffTheAnalyserAndNeverCoversIt`,
`AShortWindowShrinksTheAnalyserRatherThanTheSoundcheckStrip`,
`TheDashedMarginCurveSpansTheWholeTrustedBandNotJustItsTopEnd`).

## 2. Each item

**I-1 — allocation in `paint()`.** `lowConfidenceLabel_` and `overlayLabelFont_`
are now members built in the constructor's initialiser list, beside
`noSignalLabel_` and `tickFont_` (and placed in *declaration* order, so there is
no reorder). `SoundcheckOverlayPaintsWithoutAllocating` now asserts
`soundcheckLowConfidenceLabelForTest()` is non-empty **before any paint has
run** — that is the proof it was ctor-built. `juce::Font` has no comparable
read-back, so the test comment says plainly that the String assertion covers the
String and the initialiser list covers both.

**I-2 — the veil, measured not eyeballed.** `border` at 0.22 → **0.55**, and the
boundary line moved from `border` (the groove grey, meant to be felt not read)
to `dim`. Sampled from `console-soundcheck-results.png` at row **y = 185**, a row
with plain plot ground on both sides of the edge:

| x | RGB |
|---|---|
| 1100, 1150, 1185 (left of the edge) | **(10, 11, 13)** — the plot ground |
| 1186 | (60, 64, 70) |
| 1187 (the edge line) | **(91, 96, 104)** |
| 1188, 1250, 1400 (inside the veil) | **(29, 31, 36)** |

A **+19 / +20 / +23** lift out of 255, against round 1's ~7, plus a 2 px edge
that peaks at (91,96,104). The comment in the source now records all three
attempts and why the first two measured wrong rather than looked wrong.

**I-3 — the three comments.** `ModeRail.h`, `SoundcheckPanel.h` and
`tests/test_moderail.cpp` now say: param 2 **is** the tooltip
(`juce_TextButton.cpp:46-49`), and what ships blank is `{ {}, "LABEL" }` — an
empty NAME with the legend in the tooltip slot. The stated defence is "assert
the exact label", which is what the tests do.
**`memory/juce9-api-traps-2026-08-25.md` is untouched** — its rule is right, and
I withdraw the amendment I proposed in the round-0 report.

**I-4 — the strip wins.** New rule, in `MainComponent::resized()`: the strip asks
via `SoundcheckPanel::preferredHeight()` and **always gets it**; the analyser
goes under `kMinSpectrumHeight` if that is the cost. `preferredHeight()` is
`max(kPanelHeight, padding + lines × 19)`, so the height follows the data. The
`break` in `SoundcheckPanel::paint()` is **gone** — a fault sentence is never
dropped or half-drawn, and the comment says why the loop deliberately has no
bounds check any more. Two tests pin it (see Gap B).

**M-1 — the accent that could not paint.** `buttonOnColourId` only paints a
*latched* switch's lamp, and `ĐO` never latches, so the line set a colour nothing
could reach. Now `textColourOffId = accent`, the route `ÁP DỤNG` already takes.
Sampled (brightest pixel in each legend box):

| | `ĐO` | `BYPASS` |
|---|---|---|
| `console-live.png` (enabled) | **(255, 159, 28)** = `accentArgb` exactly | (134, 141, 152) = `dim` |
| `console-soundcheck-running.png` (locked) | (118, 82, 33) | (70, 74, 83) |

**M-2 — dashed margin curve.** `text` and `peak` are six values apart and both
read as white on a sodium plot, so the dash is what separates the measurement
from peak-hold. Implemented by subdividing each bin-to-bin segment along x (see
§3), not by stroking a dash pattern into a second Path — `createDashedStroke`
writes an outline whose element count scales with pixel length and would have
needed a second reserved member several times the size of this one.
`kSoundcheckOverlayReserveFloats` raised 3300 → **5000**, with the arithmetic
written out; the test asserts the realised count against it, so an undersized
figure fails rather than quietly reallocating.

**M-3 — the fixture.** Hot frequencies are snapped to bin centres and the
Gaussian widened from σ ≈ 0.028 to ≈ 0.1 octaves. At 247 Hz the old dip was
about 5 Hz wide against a 23.4 Hz bin: it fell *between* two bins and was never
sampled, so the marker pointed at nothing. Every one of the five ticks now has a
visible peak above it.

**M-4 — contradiction removed.** The clean-room line is suppressed when
`saturatedBins > 0` as well. A bin the deepest rung could not fix is the
opposite of a clean room. `ASaturatedBinIsNeverAccompaniedByTheCleanRoomLine`
pins both directions.

**Gap A — a landing place for `SoundcheckApplyStats`.** `Mode::Applied`, plus
`Model::placed` and `Model::clearedPrevious`, plus **`Model::worseOff()` as a
derived method, not a settable flag** — a bool beside the two counts it is
computed from is a bool that can be set to disagree with them, and the
disagreement would be invisible until a show. When true it prints first, in
`danger`: *"đã xoá N notch cũ nhưng không đặt được notch mới — phòng KÉM an toàn
hơn trước"*, and `hasError_` is set. `Applied` shows `BỎ` only (an outcome the
operator cannot clear would cover the analyser until the app restarts). Note for
Task 10: `placed` counts **lane-writes**, 2 per linked pair — not candidates.

**Gap B — the layout is pinned by tests now, not only by a picture.**
`TheSoundcheckStripTakesABandOffTheAnalyserAndNeverCoversIt` drives a real
`MainComponent` and asserts the strip never intersects the analyser, always sits
below it, gets exactly `preferredHeight()`, and that hiding it returns the
analyser's height. `AShortWindowShrinksTheAnalyserRatherThanTheSoundcheckStrip`
shrinks the window to 560 px and asserts the strip keeps every pixel it asked
for while the analyser gives way. Both call `resized()` explicitly.

## 3. My own fix broke the render, and the render caught it

The first dash implementation counted x-travel but **skipped whole bins**. The
axis is logarithmic, so two adjacent bins at 250 Hz are tens of pixels apart: one
step already exceeded a whole dash period, the state machine toggled on every
bin, and it emitted `startNewSubPath` with no `lineTo` after it. **The entire low
half of the curve — the 247 Hz hot spot the picture exists to show — drew
nothing.** The path was *full* of elements, so every existing test stayed green.

Fixed by subdividing each segment into dash- and gap-length pieces along x: the
pattern is even at 100 Hz and at 10 kHz, and no measured point is ever dropped.

Then pinned, because an element count structurally cannot see this:
`soundcheckOverlayPathBoundsForTest()` (a bounding box) and
`TheDashedMarginCurveSpansTheWholeTrustedBandNotJustItsTopEnd`.

**Mutation-checked.** Reintroducing the defect
(`while (travelled < dx && dx < kMarginDashOnPx)`), rebuilding and running:

```
1/3 Test #278: SpectrumView.SoundcheckOverlayCarriesLaneMData ..............   Passed
2/3 Test #281: SpectrumView.SoundcheckOverlayPaintsWithoutAllocating .......   Passed
3/3 Test #282: SpectrumView.TheDashedMarginCurveSpansTheWholeTrustedBand... ***Failed
67% tests passed, 1 tests failed out of 3
```

The two pre-existing overlay tests pass **under the defect**; only the new extent
test fails. Restored (`cmp` against the pre-mutation copy) and rebuilt before
committing.

One smaller thing found the same way: the extent test's own first fixture used a
*constant* margin, which draws a horizontal line — and `juce::Rectangle::isEmpty()`
is true whenever width **or** height is zero, so `ASSERT_FALSE(isEmpty())` failed
for a reason that had nothing to do with the dash. The fixture now slopes, and
the assertion is on the element count plus the extent. Noted in the test.

## 4. The pictures — re-rendered, and what I SAW in each

`build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast`

- **`D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-m-soundcheck-0915\shots\console-soundcheck-results.png`**
  — the dashed white margin curve now runs the **full** trusted band from ~85 Hz
  to the 6 kHz edge with even dashes throughout, and each of its five broad peaks
  sits directly above a red floor tick (247 Hz included, which round 1 had
  pointing at nothing); the low-confidence veil is plainly visible right of the
  edge with a bright boundary line and a legible `độ tin cậy thấp`; the four
  summary lines (`tìm thấy 5 điểm dễ hú` / saturated+action / `1 kênh không đo
  được` / `1 kênh sai định tuyến`) all fit with the headline one size up;
  `ÁP DỤNG` and `BỎ` untruncated; frequency axis and the whole plot intact.
- **`…\shots\console-soundcheck-running.png`** — strip below the plot, axis
  intact, `ĐANG ĐO · KÊNH 2/4` over `48 s`, red `DỪNG`; all four mode switches
  grey-locked while `ĐO` is a *dimmed amber* — visibly its own control even when
  locked, which it was not before M-1.
- **`…\shots\console-live.png`** — `ĐO` is now sodium amber (255,159,28), the only
  accent legend on the rail, hint `phát tín hiệu` untruncated; everything below
  the transport unchanged.
- **`…\shots\console-idle.png`** — `ĐO` enabled and amber, panel hidden, analyser
  at full height, both empty states intact.
- **`…\shots\console-preset-music.png`** — unchanged apart from the `ĐO` cell;
  still shows Q 25 / −10 dB as text in the off-list combos.

## 5. Still open

1. Task 10 must feed the panel `SoundcheckController::getRemainingMsInRun()` and
   **not** `NotchController::getSoundcheckRemainingMs()`. The substitution
   compiles and passes everything here (F12); it needs its own test in Task 10.
2. `Mode::Applied` exists and is tested but is **not reachable from the app** —
   nothing calls it yet. That is Task 10's job, and it is now a wiring change
   rather than a design one.
3. On a console shorter than the strip itself the analyser can reach zero height.
   That is the deliberate direction of the I-4 trade, not an oversight.
4. `ĐO`'s hint is Vietnamese while the three mode hints beside it are English —
   still mixed in the render; an owner call, not a defect.

---

# Task 9 - fix report, review round 2

**Commit `c6dd05c`** (on `7ef8848`). Model: **Claude Opus 5 (1M context)**, `claude-opus-5[1m]`.
**Expected level change: 0 dB.** No audio path touched. One item, the only one raised.

## The defect

`kSoundcheckOverlayReserveFloats` was derived from an assumed 2560 px maximum
plot width. **Nothing enforces such a maximum** - `src/main.cpp:59-61` calls
`setResizeLimits(MainComponent::kMinimumWidth, MainComponent::kMinimumHeight,
16384, 16384)`. Because the dash period was in *absolute pixels*, the dash count,
and with it `soundcheckOverlayPath_`'s element count, grew with the plot width.
Past the reservation, `juce::Path` reallocates **inside `paint()`** - the one
thing this view is built not to do, and the rule round 1 had just been fixed for.

## The fix - remove width from the arithmetic

The dash is now a **fixed number of periods across the plot**:
`kMarginDashPeriods = 110` with `kMarginDashDutyCycle = 7/12`. The per-plot
lengths are derived in `paintSoundcheckOverlay` from `plot.getWidth()`, so the
*count* is a constant at every window size. Both terms of the reservation are
then constants:

```
1025 bins  (Detector::kNumBins, worst case all trusted and in range)
+ 110 dashes x 2 elements (startNewSubPath + the lineTo that ends it mid-segment)
= 1245 elements x 3 coords = 3735
-> kSoundcheckOverlayReserveFloats: 5000 -> 4200   (headroom, not a wider guess)
```

An untrusted run costs nothing extra: the next dash's `startNewSubPath` is
already in the 220. The derivation is written out in the header, and it no
longer names a window size.

I did **not** touch the resize limits.

## Measured, not argued

`SoundcheckOverlayPaintsWithoutAllocating` now also paints at **8192 px** -
double the widest display sold, half what `main.cpp` permits - and asserts both
the ceiling **and** that the count is not materially larger than at 900 px. A
ceiling-only assertion would still pass a version that merely happened to fit.

```
elements @900px = 605    @8192px = 605     <- identical; width is gone
floats   @8192px = 1815  reserve = 4200
```

**Mutation-checked** by restoring the absolute 12 px period:

```
elements @900px = 538    @8192px = 1718
floats   @8192px = 5154  reserve = 4200
  -> Expected: (elementsWhenVeryWide * 3) <= (kSoundcheckOverlayReserveFloats),
       actual: 5154 vs 4200
  -> Expected: (elementsWhenVeryWide) < (elementsAfterFirst * 2),
       actual: 1718 vs 1076
  [  FAILED  ] SpectrumView.SoundcheckOverlayPaintsWithoutAllocating
```

Both new assertions fire. Restored (`cmp` against the pre-mutation copy), the
temporary measurement probe removed, and rebuilt before committing.

## Verification

```
cmake --build build --config Release            # 0 error C / error LNK
cd build && ctest -C Release -R "SpectrumView" --output-on-failure
  100% tests passed, 0 tests failed out of 26
cd build && ctest -C Release
  100% tests passed, 0 tests failed out of 689
```

689 is unchanged from round 1: the extension lives inside an existing test.

## The picture

`D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\.claude\worktrees\lane-m-soundcheck-0915\shots\console-soundcheck-results.png`
- indistinguishable from round 1 at 1440 px, which is the intent: the fix only
changes behaviour on windows wider than the console was ever rendered at. Dash
density measured off the PNG rather than eyeballed, by counting near-white runs
along the curve:

| span | dashes | on-lengths (px) | gap-lengths (px) |
|---|---|---|---|
| 85-250 Hz (the stretched low end) | 11 | 7, 7, 7, 7, 7, 7, 7, 7 | 6, 5, 5, 6, 5, 6, 5 |
| 2.5-6 kHz (the dense high end) | 14 | 7, 8, 7, 8, 7, 8, 6, 8 | 5, 5, 5, 5, 5 |

One density across the whole plot, and the same 7-on / 5-off the absolute period
produced. Read back in full as well: the curve still spans the entire trusted
band, its five peaks still sit above their red floor ticks, the veil and
`do tin cay thap` are unchanged, and the four summary lines and `AP DUNG` / `BO`
are untouched.

The other four PNGs are unaffected - the overlay is the only thing this commit
changes, and it appears in no other scene.
