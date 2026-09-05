# Merge `main` (lane P) into lane S — report

Merge commit `6f9a723`, parents `3d0481a` (lane S) + `2e7f232` (main).
Worktree: `.claude/worktrees/project-overview-update-44a2ea`, branch
`claude_desk/feedback-detection-upgrade-102019`. Not pushed.

## 1. Textual conflicts

### `CMakeLists.txt`
`project(HandsFree VERSION 1.1.0)` kept (lane S). main's 1.0.5 dropped. The
release script bumps to 1.1.1 on the next drop, so nothing else needed touching
— the version lives in exactly one place and `handsfree.nsi` `!searchparse`s it.

### `memory/MEMORY.md`
Union of both sides, main's line last:

- lane S: `anti-feedback-research-2026-09-04`, `stereo-lane-lessons-2026-09-05`
- main:   `sdd-workspace-gitignore-trap-2026-09-04`

Also fixed **outside** the conflict: the `preset-save-roundtrip-2026-09-05`
index line still ended "dedup theo lane khi đọc snapshot ngược ra
`PresetNotch`" — main's line, auto-merged clean, made false by §2 below. It now
states the per-(slot, làn, index) rule. The note file itself (§3 of it) was
rewritten the same way; it now leads with the merge lesson rather than the
dedup that caused it.

### `docs/KY-THUAT-CHONG-HU.md` (three hunks + one non-conflicted fix)

- **§4** — kept lane S's `lane`/`linked` sentence AND main's "Trạng thái nối dây
  (05/09/2026, chuỗi preset trọn vẹn)" paragraph. Its "dedup theo lane" phrase
  is gone; the paragraph now says SAVE writes one notch per (slot, làn, index)
  with `lane`, plus a `"slots"` section carrying routing and `linked`.
- **§4 loader paragraph** (auto-merged clean, but stale after §2): the ">16
  notch hoặc trùng index → từ chối" sentence now reads per (slot, làn), states
  that two notches sharing an index on different lanes are legal, and that
  `lane` = −1 collides with any lane at that index.
- **§7 heading** — 05/09/2026, "v1.1.1 — chờ release", suite **402/402**
  (measured, not carried over), keeping main's "+2 test cho nút LOAD/SAVE" and
  adding this merge's +1 round-trip and +2 validator tests.
- **§7 "đã hạ cánh"** — merged: lists the GUI console rebuild, the full preset
  chain AND lane S. The open list drops "nối GUI nạp/lưu preset + seed
  first-run" (shipped) and keeps code signing, hardware integration testing,
  RING RISK data, and the `laneAsymmetryBonus` sweep.

## 2. The semantic conflict — `MainComponent::savePreset`

main's loop auto-merged clean and was wrong. It collapsed the two lanes of a
slot into one `PresetNotch` keyed by (slot, index), keeping the lowest channel,
and never wrote `lane`. That was correct *while lane P owned the file alone*:
detection was mono and `adoptPreset` mirrored every notch onto both lanes, so
one entry genuinely described both. Under lane S with INDEP as the default the
two lanes carry different notches at the same chain index, so the dedup
silently dropped every lane-1 notch. It also wrote no `slots` section at all,
so slot routing and the per-slot LINK flag never reached the file.

**Now** (`src/app/MainComponent.cpp`):

- one `PresetNotch` per snapshot notch, `pn.lane = sn.channel`, `pn.slot = s`,
  index/freq/Q/depth unchanged, **no dedup**;
- a `slots` section: every slot the engine reports `enabled` contributes
  `PresetSlot { index = s, config = engine_.getSlotConfig(s), linked = isSlotLinked(s) }`;
- sample-rate logic (first non-zero `snap.sampleRate`, 0 → `saveToFile` refuses)
  and the refusal log are untouched;
- the comment block explains the per-lane rule and points at spec S.

`loadPreset` on the merged tree already applies `setSlotLinked(entry.index,
entry.linked)` before `setWidth` — verified, the merge kept lane S's line (in
the `result.preset.slots` loop). main's LOAD…/SAVE… button wiring compiles
against the lane S `MainComponent` unchanged.

### `PresetManager::validate` — uniqueness and the count limit

Both rules were global over the notch list: `> MAX_NOTCHES` entries total, and
a `std::set<int>` of `index`. Widened to key on (slot, lane, index):

- `lanesPerSlotIndex : (slot,index) -> set<lane>`. A clash is: the same lane
  twice, an every-lane (`-1`) entry already present, or this entry being `-1`
  with anything already there.
- `notchesPerSlotLane : (slot,lane) -> count`, refused above `MAX_NOTCHES`. A
  `lane == -1` entry consumes its index on **both** lanes and counts against
  both — that is what keeps `MoreNotchesThanTheChainHasSlotsIsRefused` (17
  laneless notches on slot 0) failing as it should.

Decision `[S]` in `src/app/PresetManager.h` updated to match. `<map>` added to
the includes. Both pre-existing tests stay green unchanged: their notches carry
no `slot` and no `lane`, so they land on (0, −1) and are refused exactly as
before.

### The one existing test whose assertion had to change

`GuiWiring.SavePresetWritesAFileThatLoadPresetReopensIdentically` asserted
`notches.size() == 1u` with the comment "savePreset dedups across lanes" — it
encoded the defect. It now expects 2 entries, one per lane, same params, and
asserts both lanes appear.

## 3. Tests added, with RED evidence

All three RED runs were produced by temporarily mutating production and
rebuilding; the good sources were restored from a scratchpad copy afterwards
and the full suite re-run green before the commit.

### `GuiWiring.SavePresetRoundTripsPerLaneNotchesAndTheLinkedFlag`
`tests/test_gui_wiring.cpp`. Stereo slot 0 in INDEP with index 2 on lane 0 at
1 kHz and index 2 on lane 1 at 2 kHz; `setSlotLinked(1, true)` on slot 1; a
block pumped so `runOnce()` publishes; `savePreset` to a temp file; a **fresh**
`MainComponent` loads it; asserts controller 0's snapshot carries both notches
on their own lanes at their own frequencies and `isSlotLinked(1)`. The temp
file is removed by an RAII `FileGuard`, so it goes on every path including a
failed `ASSERT_*`.

RED with main's dedup + no `slots` section restored:

```
tests\test_gui_wiring.cpp(984): error: Value of: rightFound
  Actual: false / Expected: true
lane 1's 2 kHz notch at index 2 did not survive the round trip
tests\test_gui_wiring.cpp(986): error: Value of: reopened.isSlotLinked (1)
  Actual: false / Expected: true
the saved "slots" section did not carry `linked`
[  FAILED  ] GuiWiring.SavePresetRoundTripsPerLaneNotchesAndTheLinkedFlag
```

(`SavePresetWritesAFileThatLoadPresetReopensIdentically` went red in the same
run, on the 2-notch assertion.)

### `PresetManager.DuplicateIndexOnDifferentLanesIsAccepted`
Red if uniqueness goes back to keying on `index` alone (or on (slot, index)) —
the file every stereo INDEP slot now produces would be refused.

RED with the old global-`index` validator restored:

```
tests\test_presetmanager.cpp(413): error: Expected equality of these values:
  result.preset.notches.size()  Which is: 0
  2u                            Which is: 2
[  FAILED  ] PresetManager.DuplicateIndexOnDifferentLanesIsAccepted
```

(the two `GuiWiring.SavePreset*` tests also went red in that run, because the
old validator refuses the per-lane file `savePreset` now writes — 3 failed of 5)

### `PresetManager.DuplicateIndexOnLaneMinusOneAndALaneIsRefused`
Red if the per-lane widening treats `-1` as just another lane value.

RED with the clash test reduced to `lanes.count (notch.lane) > 0`:

```
[  FAILED  ] PresetManager.DuplicateIndexOnLaneMinusOneAndALaneIsRefused
0% tests passed, 1 tests failed out of 1
```

## 4. Spec

`docs/superpowers/specs/2026-09-05-stereo-aware-detection-design.md`:

- **§4.6** last paragraph: "App hiện không có đường lưu preset … cho đến khi
  lane P" → "Hợp nhất với lane P ngày 05/09/2026": `savePreset` writes one notch
  per (slot, làn, index) with `lane`, plus a `"slots"` section with `linked`;
  validator uniqueness is per (slot, làn, index) and `lane` = −1 collides with
  any lane at that index.
- **S-10** row struck through with the same replacement text. Everything else
  in the spec is untouched.

## 5. Build and suite

```
cmake --build build --config Release            # clean, no errors
ctest -C Release -R "PresetManager|MainComponent|NotchController"
  100% tests passed, 0 tests failed out of 136
ctest -C Release
  402/402 Test #402: PresetFirstRun.ReportsAMissingSourceInsteadOfFailingSilently ... Passed 0.02 sec
  100% tests passed, 0 tests failed out of 402
  Total Test time (real) =  23.71 sec
```

402 = the union of main's 368 and lane S's 397, plus the 3 tests added here.
(The `-R` filter above does not catch the `GuiWiring.*` cases; the full run
does.)

## 6. Files in the merge commit

31 files, 1987 insertions, 135 deletions. Resolved or reconciled by hand:

```
CMakeLists.txt                                     (conflict; 1.1.0 kept — no diff vs lane S)
memory/MEMORY.md                                   (conflict, union + stale index line)
docs/KY-THUAT-CHONG-HU.md                          (conflict, 3 hunks + loader paragraph)
src/app/MainComponent.cpp                          (savePreset rewritten)
src/app/PresetManager.cpp                          (validate() per (slot, lane, index))
src/app/PresetManager.h                            (decision [S])
tests/test_gui_wiring.cpp                          (1 test updated, 1 added)
tests/test_presetmanager.cpp                       (2 tests added)
docs/superpowers/specs/2026-09-05-...-design.md    (§4.6, S-10)
memory/preset-save-roundtrip-2026-09-05.md         (§3 rewritten)
```

The rest came from main unchanged (installer, DeviceDrawer, plans, TESTER-NOTES,
`.superpowers/sdd/2026-08-27-next-wave/`, `skills-lock.json` deletion, CLAUDE.md,
GIOI-THIEU.md).

`.superpowers/sdd/.gitignore` (`*`, auto-written by the SDD script) was present
and untracked; deleted before committing, per main's
`memory/sdd-workspace-gitignore-trap-2026-09-04.md`. The lane S SDD workspace
was already gone and was not recreated. `git status` is clean.

## 7. Concerns

1. **No listen test.** This changes what a saved preset contains, not the DSP
   path — no coefficient, gain-staging or buffer change, so no level change is
   expected on any signal. The suite is the only evidence here, and per the
   project's own rule that is not proof of stage behaviour. It is preset I/O,
   so the risk is a lost tuning rather than a loud noise, but a tester should
   still do one save/reload on a real rig before the next drop.
2. **Files written by 1.1.x are not readable-as-intended by 1.0.5.** Version
   stays `"1.0"` and old builds ignore unknown keys, so a 1.0.5 build opening a
   1.1.x file will see two notches at the same index on one slot and refuse it
   (its validator is still global-by-index). Forward compatibility was already
   broken by lane S's format additions; this makes it reachable in practice
   because SAVE now emits `lane` routinely. Worth a line in TESTER-NOTES if any
   tester is still on 1.0.5.
3. **`savePreset` only writes ENABLED slots.** A slot the user configured but
   disabled loses its routing and `linked` on save. That matches "the slots
   section describes what is running", and `slotIsNonDefault` in `saveToFile`
   would otherwise emit noise, but it is a behaviour choice worth confirming
   with the owner rather than an obviously correct one.
4. **The count limit is now per (slot, lane), so a file may hold up to
   8 × 2 × 16 = 256 notches.** That is the honest capacity of the chain array,
   but the old global 16 was also acting as an accidental sanity cap on file
   size. No other limit replaced it.
5. **`CMakeLists.txt` shows no diff in the merge commit** because lane S already
   carried 1.1.0. Anything grepping the commit for the version resolution will
   not find it — it is recorded in the commit message only.
6. The release script has NOT been run; nothing was packaged or dropped to
   `Z:\My Drive\RELEASE\ALPHA TEST`, and nothing was pushed.
