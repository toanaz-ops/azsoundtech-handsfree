# Lane report — Preset format (Task 25 format half, Task 26)

**Branch:** `claude_desk/gifted-yonath-6714d4` (worktree
`.claude/worktrees/gifted-yonath-6714d4`), based on `main` @ `01db1f0`.
**Brief:** [lane-presets.md](lane-presets.md). **Status:** complete, uncommitted
at the time of writing.

## Measured, not asserted

```
cmake --build D:\hf-lanes\bld-presets --config Release      -> 0 errors, both targets
  HandsFree.vcxproj      -> ...\Release\AZ Soundtech Hands-free.exe
  HandsFreeTests.vcxproj -> ...\tests\Release\HandsFreeTests.exe

ctest --test-dir D:\hf-lanes\bld-presets -C Release
  100% tests passed, 0 tests failed out of 121
```

Baseline before this lane, measured in this same worktree: **75/75**. So the
121 is 75 unchanged plus **46 new**. Nothing in `src/dsp/`, `AudioEngine.*`,
`MainComponent.*` or `src/gui/` was touched, so the 75 could not have moved —
and did not.

## What was built

| File | |
|---|---|
| `src/app/PresetManager.h` | New. Format types and the static API. |
| `src/app/PresetManager.cpp` | New. |
| `presets/Speech.json`, `presets/Music.json` | New. Task 26. |
| `tests/test_presetmanager.cpp` | New. 46 tests. |
| `CMakeLists.txt` | +2 lines (the new sources). |
| `tests/CMakeLists.txt` | +1 source line, +1 `target_compile_definitions` block. |

`PresetManager` references no engine and no DSP type. It translates between a
plain `Preset` struct and JSON, and nothing else.

## Location of the default presets

**`presets/` at the repo root.** Not `%APPDATA%` — that is where a *user's*
presets live, and a shipped default that only exists after first run cannot be
tested in CI. The installer copies them into
`%APPDATA%/AZSoundtech/HandsFree/presets/`; wiring that copy into app startup
touches `main.cpp`/`MainComponent`, which this lane does not own, so **it is
not done** (see "Not done" below).

The tests read the real shipped files off disk through the real loader, via
`HANDSFREE_PRESET_SOURCE_DIR` passed in from `tests/CMakeLists.txt`. A typo in
either file is a red test, not a dialog on a soundman's laptop.

## The four open questions the brief asked me to decide

**1. Version mismatch → REFUSE, quoting the version it did not understand.**
A strict allow-list (currently `{"1.0"}`), not a `>=` comparison. A version
failure returns *immediately* without validating the remaining fields, because
their meaning is precisely what is in doubt. When a `1.1` exists it is added to
the allow-list together with whatever migration it needs. Refusing costs one
dialog; guessing at a format we do not know costs a filter we cannot predict.

**2. A preset saved at a different sample rate → the check is split in two.**
This is the decision I am most confident about and the one the brief stated
least precisely.

- *Validation* compares `freq` against the sample rate **the file itself
  stores**. A file whose notch sits above its own Nyquist could never have been
  written by this program → the file is corrupt → refuse.
- *Applicability* against the **currently open device** is a separate, pure
  query: `planFor (preset, targetRateHz)`. A 96 kHz session reopened at
  44.1 kHz is a normal Tuesday, not a corrupt file.

`planFor` implements **D-00**: a notch at or above the target Nyquist comes
back `AboveNyquist` with its frequency, Q and depth **exactly as stored**, and
stays in the list. Never clamped (that is the measured 4.11e18 defect) and
never dropped (that would discard exactly what D-00 says to keep). A target
rate of `<= 0` — no device open — makes every notch `AboveNyquist`.

**3. More than 16 notches, or a duplicate `index` → REFUSE, both.** Slots are
index-addressed. Letting the later of two duplicates win means the preset the
user hears is not the preset in the file.

**4. A `device` name that is not present → LOAD ANYWAY.** Metadata, never a
gate. The stored name survives verbatim so the caller can name the interface
the preset came from. The `device` *key* is still required to be present and a
string — a missing key is a malformed file; an unrecognised value is a normal
Tuesday.

## A fifth decision the brief did not anticipate — please review this one

**Plan Task 26 is not expressible in the Task 25 format, and the brief did not
notice.**

Task 26 asks for `Speech.json` (Q=40, −18 dB) and `Music.json` (Q=25, −10 dB).
Spec §7 defines a preset as *"danh sách notch đã lock + audio device + buffer
size"* — the notches **already locked**. A preset shipped in an installer has
locked nothing. So under the format as written, both files reduce to
`"notches": []` and are byte-identical apart from their filenames; Q=40/−18 and
Q=25/−10 appear nowhere, and a test that "loads both defaults" asserts nothing.

The brief's claim that these values "were not representable until this session"
is true about **depth as a filter parameter** (commit `0eab329`) but does not
address the separate problem that a preset with no notches has nowhere to put a
Q or a depth at all.

Resolved **with the owner in-session**: the format carries an **optional**
`notchDefaults` block.

```json
"notchDefaults": { "Q": 40.0, "depth": -18.0 }
```

Optional is the load-bearing word — the plan Task 25 sample has no such key and
still parses (there is a test asserting exactly that), and when the block is
absent the documented fallbacks apply: Q 30, depth −12, which are the values in
the plan's own sample notch and in the spec §6.1 notch table. The block is
validated by the same rules as a real notch, because a bad default is worse
than a bad notch: it seeds every notch the detector goes on to create.

This is the **only** place this lane extended the format rather than
implementing it. Additive, reversible, and overturnable without touching any
other decision above.

Consequence, deliberately: **the shipped defaults lock no notches**, and a test
asserts it. A default carrying notches at frequencies nobody measured would cut
real dB at a guessed frequency on every PA it is loaded onto.

## The new tests, and what breaks each

46 tests in one suite. Rather than list all 46, here are the load-bearing ones
with the production change that makes each fail — and **three of these were
verified by actually making that change, rebuilding, and watching the named
test go red**, not by reading the code.

| Test | Production change that breaks it | Verified by mutation |
|---|---|---|
| `DepthKeepsItsNegativeSignThroughTheFile` | `std::abs()` on `depthDB` in the writer | **yes** — 4 red (this one plus the 3 round-trip tests), 42 green |
| `ANotchAboveTheTargetNyquistIsPlannedIdleWithItsParametersIntact` | `planFor` clamping `freq` down to fit the target Nyquist | **yes** — exactly 1 red, 45 green |
| `AFrequencyAtOrAboveTheStoredNyquistIsRefused` | deleting the Nyquist branch in `validateNotchValues` | **yes** — exactly 1 red, 45 green |
| `SampleRateIsWrittenAsANumberNotADisplayString` | reaching for `getCurrentSampleRate()` (`"48000 Hz"`) instead of `getCurrentSampleRateHz()` | by inspection |
| `SavingRefusesAPresetItsOwnLoaderWouldRejectAndWritesNothing` | `saveToFile` skipping `validate()` | by inspection |
| `APositiveDepthIsRefused` | dropping the `depthDB > 0` rule | by inspection |
| `ABooleanWhereANumberBelongsIsRefused` | an `isInt()`-style check that lets `var(true)` through as 1 | by inspection |
| `EveryProblemIsReportedNotJustTheFirst` | short-circuiting on the first fault | by inspection |
| `ADuplicateIndexIsRefused` | last-one-wins instead of refusing | by inspection |
| `ANonIntegralBufferSizeIsRefused` | `static_cast<int>` truncating 64.5 silently | by inspection |
| `ThePresetDirectoryIsUnderTheUserApplicationDataDirectory` | hard-coding a path instead of asking JUCE | by inspection |
| `TheShippedDefaultsLockNoNotches` | pre-seeding the defaults with guessed frequencies | by inspection |
| `static_assert` in the test file | `PresetManager::MAX_NOTCHES` drifting from `NotchChain::MAX_NOTCHES` | compile error, by construction |

Malformed-input coverage, as the brief required: truncated, empty, valid JSON
that is not a preset, a bare JSON string, a `notches` array containing a
string, `notches` that is not an array, a missing key, a quoted number, a
boolean where a number belongs, `depth > 0`, `freq` above Nyquist, `freq <= 0`,
`Q <= 0`, `sampleRate <= 0`, a non-numeric `sampleRate`, a non-integral
`bufferSize`, `index` out of range, and a duplicate `index`.

## The hand-off to the wiring half of Task 25

As data, never as an action — D-05 gives the detector sole ownership of the
notch model, so nothing here may reach into `AudioEngine` or `NotchChain`.

```cpp
// 1. Read. Never throws; every problem is a message.
PresetLoadResult r = PresetManager::loadFromFile (file);
if (! r.ok) { showTheUser (r.errors); return; }

// 2. Decide, against the rate the device is ACTUALLY running at.
std::vector<PlannedNotch> plan =
    PresetManager::planFor (r.preset, engine.getCurrentSampleRateHz());

// 3. The future NotchController hands `plan` to the DETECTOR, which under
//    D-05 owns adoption and auto-release. Applicable  -> install.
//    AboveNyquist -> register Idle, parameters intact, per D-00.
```

`PlannedNotch { PresetNotch notch; NotchApplicability applicability; }` where
`NotchApplicability` is `Applicable | AboveNyquist`. `PresetNotch` is a plain
aggregate of `int index; double freq, Q, depthDB;` — no JUCE type, no engine
reference, nothing that needs an audio device to construct.

`r.preset.notchDefaults` is what the detector should give a **new** notch it
creates while this preset is loaded.

## Corrections to the brief and the runbook

1. **The Task 26 / spec §7 gap above.** The largest finding in this lane.
2. **Everything else in the brief checked out.** `getCurrentSampleRateHz()`
   exists (`AudioEngine.h:120`) and its display-string sibling is the trap the
   brief says it is; `Biquad::setNotchFilter`'s four-argument form refuses
   `depthDB > 0` (`Biquad.cpp:98`) and accepts `depthDB == 0` as a documented
   no-op, so the loader is exactly as strict as the filter and no stricter;
   `NotchChain::MAX_NOTCHES` is 16.
3. **The runbook generalises to harness-created worktrees.** This lane's
   worktree is under `.claude/worktrees/`, not `D:/hf-lanes/` as
   `parallel-lanes.md` §3 assumes. The junction step and both `git config
   --worktree` lines apply verbatim, and the documented failure reproduced
   exactly — `fatal: not a git repository:
   external/JUCE/../../.git/modules/external/JUCE` — until they were applied.
   The main repo's `git submodule status` was checked afterwards and still
   leads with a space. Build directory kept outside the repo at
   `D:/hf-lanes/bld-presets` per §5.
4. **Configure took 299 s here**, not the ~71 s §4 records. Same generator
   (`Visual Studio 18 2026`), cold googletest fetch. Not a defect, but budget
   for it.

## Not done, and why

- **The wiring half of Task 25.** Blocked by D-05 on a detector that does not
  exist. This was the brief's own scope boundary.
- **"Load on first run" (the second half of Task 26).** Copying the shipped
  defaults into `%APPDATA%` at startup, and populating the preset dropdown,
  both require `main.cpp` / `MainComponent`, which the GUI lane owns and is
  live in right now. `PresetManager::getPresetDirectory()` is there and tested,
  so the copy is a few lines for whoever owns startup.
- **`memory/MEMORY.md` not updated.** Three sibling lanes are live against the
  same repo and it is the shared index file; per
  `az-harness:parallel-session-coordination` that edit should happen once, when
  the lanes land, rather than four times in conflict.
- **No commit made.** Awaiting approval.
