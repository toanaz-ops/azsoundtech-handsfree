# Final fix wave — lane M whole-branch review

**Date** 2026-09-16 · **Branch** `feat/lane-m-active-soundcheck` · **Base** `f9dbad3` (712/712)
**Model** Claude Opus 5 (1M context), `claude-opus-5[1m]` — commit trailer per the
dispatching session's convention.
**Worktree** `.claude/worktrees/lane-m-soundcheck-0915`

Single implementer, one wave, covering the final whole-branch review's findings
plus the independent verifier's warning count.

**Result: 714/714 on a clean configure + Release build, zero warnings in lane-M
files, all three Important fixes mutation-checked.**

---

## 1. What changed

### I-1 (Important) — the confirmation did not say detection goes off

`arm()` calls `setDetectionActiveOnAllSlots(false)`: **every** slot, for the
**whole** run, not just the channel being swept. On a 16-channel rig that is
~72 seconds with no feedback protection anywhere. The dialog asked the operator
to drop their master and said nothing about it.

- `src/app/MainComponent.cpp` — new `kScConfirmDetectionOff` beside
  `kScConfirmTotal2`, appended in `soundcheckConfirmText()`. Leading `\n\n` so
  it reads as its own paragraph, not as a footnote to the duration. UTF-8 as
  hex escapes, the convention every neighbouring label already uses:

  > Trong suốt phép đo, bộ chống hú TẮT trên mọi slot. Nếu phòng bắt đầu hú, bấm DỪNG ngay.

- `tests/test_gui_wiring.cpp` —
  `MainComponentSoundcheck.TheConfirmationSaysHaMasterTruocAndTheRealDuration`
  extended with two assertions: the "bộ chống hú TẮT trên mọi slot" clause, and
  that `DỪNG` is named.
- Docs: `docs/GIOI-THIEU.md` ĐO section, `installer/TESTER-NOTES.md` item 15.

### I-2 (Important) — the ring-risk gate read only the displayed slot

Preflight (`~:1292`) and arm (`~:1376`) both copied
`notchControllers_[displayedSlot_]`. But `arm()` disarms **every** slot and
`buildSoundcheckTargets()` sweeps **every enabled** slot, so a room ringing on
slot 1 while the console showed slot 0 was armed and swept — with the one
detector that could have caught the howl switched off a moment later.

- `src/app/MainComponent.{h,cpp}` — new
  `worstRingRiskSnapshot (int& slotOut) const`. Iterates every **enabled** slot
  **plus the displayed one**, `copySnapshot`s each, skips
  `ringRiskValid == false`, and keeps the highest `ringRiskScore`.

  Two deliberate choices:
  - **The displayed slot stays in the set even when disabled.** That makes the
    old read a strict subset of the new one, so this gate can only ever refuse
    *more* runs, never fewer. A safety gate is not a place to trade coverage for
    tidiness (global rule 10).
  - **`ringRiskValid == false` is skipped, not compared.** An unscored detector
    publishes score `0.0f`; letting that win a `max()` would hide a real reading
    behind a slot that has never measured anything.

- `src/app/SoundcheckController.{h,cpp}` — `arm()` takes
  `int riskSlot = -1`, a **log label only** (the refusal identity still reads
  the snapshot and nothing else). `soundcheck_start` gains `ring_risk_slot`,
  `null` when nothing has scored — the same null-vs-zero discipline `ring_risk`
  already follows. Defaulted, so every existing `arm()` call site compiles
  unchanged.
- Test: `MainComponentSoundcheck.RingRiskOnAnUndisplayedSlotRefusesTheRun` —
  slot 1 driven to Critical, console showing slot 0, slot 0's detector asserted
  to have **no** valid snapshot (so the old gate saw nothing), preflight must
  refuse and the dialog must never open.
- Docs: `docs/KY-THUAT-CHONG-HU.md` §3.7, gate description.

### I-3 (Important) — `worseOff()` was computed on cross-slot totals

`clearedPrevious` and `placed` reach the strip summed over every slot, and the
sum hides exactly the outcome `worseOff()` exists to catch: slot 0 clears 3 and
places 0 while slot 1 places 6, and `3 cleared / 6 placed` reads as an
unqualified success. Slot 0's chain is the one that howls.

- `src/gui/SoundcheckPanel.h` — `Model::anySlotWorseOff` (a field, not a
  derivation: the two counts it would be derived from are precisely the ones the
  sum destroyed) and `Model::showWorseOff() = worseOff() || anySlotWorseOff`.
  `worseOff()` itself is untouched. `kMaxSummaryLines` 6 → 7.
- `src/gui/SoundcheckPanel.cpp` — `hasError_` and the danger line read
  `showWorseOff()`. The "đã đặt N notch mới" line is no longer an `else`: when
  one slot regressed and the totals still placed something, **both** lines are
  true and both print. It is suppressed only when nothing was placed anywhere,
  where the danger line already says everything a "đã đặt 0" line would.
- `src/app/MainComponent.cpp` — `applySoundcheckProposals()` accumulates
  `bool anySlotWorseOff` per slot (`stats.clearedPrevious > 0 && stats.placed == 0`
  for THAT slot), feeds it into the model, and the `showMessage` sentence follows
  the same predicate as the strip.
- Test: `SoundcheckPanel.AWorseOffSlotIsNotMaskedByAnotherSlotsSuccess`.

### M-1 — non-portable noise fixtures

`std::mt19937`'s sequence is standardised; `std::normal_distribution`'s mapping
of it is **not**, and MSVC and libc++ use different algorithms. The fixtures'
flake guards were therefore only ever measured on MSVC.

`whiteNoise()` (`tests/test_soundcheckcontroller.cpp`) and `noise()`
(`tests/test_loopgainestimator.cpp`) now build a Box–Muller pair from
`std::uniform_real_distribution` on the same seed. Same statistics (mean 0,
s.d. sigma, independent), and every major implementation of
`uniform_real_distribution` agrees. `u1` is floored at `1e-12` so `log(0)` can
never put an inf into a fixture.

**The guards are unchanged.** Measured on this machine (see §3 for the raw
output), before → after:

| fixture | before (`normal_distribution`) | after (Box–Muller) | guard | margin |
|---|---|---|---|---|
| quiet noise floor, sigma 1e-3, seed 3 | 7.35 quoted / **1.431825** measured | **1.450228** | `0 < x < 10.0` | 6.9× below the gate |
| derived-A tone, target peakiness 10 | **9.753669** | **9.509144** | `5.0 < x < 20.0` | 1.90× above / 2.10× below |

### M-3 — test seam hardening

`AudioEngine::setSoundcheckGainUnclampedForTest` now returns early on a
non-finite gain (`std::clamp` on a NaN returns the NaN, and this seam is the one
route by which an unclamped amplitude reaches the sweep path). Refusing leaves
the previous finite value standing, so a fat-fingered test fails on its own
assertion instead of poisoning the sweep. `AudioEngine.h` carries the line
**"TEST SEAM — no production caller; grep before adding one"**; verified — the
only caller in the tree is `tests/test_audioengine.cpp:1410`.

### M-5 — stale constant in a comment

`tests/test_soundcheckcontroller.cpp:~696` named `kNoiseFloorGuardMs`, which
does not exist. The lead-in it means is `kSweepLeadInMs`.

### M-6 — unbraced `else`

`src/app/AudioEngine.cpp:~962`, the `else` before the nested tap-write loop, is
now braced. **Braces only** — the callback diff carries no reindent and no other
change, so the audio path is verifiable at a glance.

### M-2 — one clause on the mic-capture clear

`SoundcheckController.cpp:~813` claimed "capture is OFF at this point, so
nothing is writing". True, but only because of the Gap: the callback snapshots
`scCaptureActive_` **once** at the top of its block, so it is one block behind
the store, and a callback already in flight when `enterGap()` ran can write one
more block after it returned. `kGapMs = 300 ms` is what stands between that late
block and the `clear()`.

### VERIFIER — `warning C4459` in `SoundcheckPanel.cpp:302`

`addSummaryLine`'s parameter `text` shadowed `az::theme::text` (a Colour), which
this TU pulls in with a file-scope `using namespace az::theme;`. Renamed to
`line` in both the header and the definition. Gone from the clean build.
`task-10-report.md` has an appended correction to its three "no new warnings"
claims.

### Spec citations

`docs/superpowers/specs/2026-09-15-active-soundcheck-design.md` §4.6(f) and the
§6 coverage table cited `NotchController.cpp:1116`. Task 11 counted `:1117` (the
`origin != Origin::Soundcheck` memory gate) and `:1170` (the Soundcheck ceiling
override) on this branch; both verified by opening the file. Corrected, with a
note. The round-2 review-log rows in §9 keep `:1116` — they record what the
reviewer said at the time, not a live claim.

---

## 2. Commands and output

### Clean configure + Release build

```
$ rm -rf build
$ cmake -B build -G "Visual Studio 18 2026" -A x64
-- ASIO SDK found at .../external/asiosdk
-- Configuring done (39.5s)
-- Generating done (0.2s)
-- Build files have been written to: .../build

$ cmake --build build --config Release
exit=0
```

**Errors: 0. Warnings: 38, and not one of them in a lane-M file.** By file:

| count | file | warning |
|---|---|---|
| 28 | `src/gui/NotchListPanel.cpp` | C4244 float→int (pre-existing) |
| 6 | `src/dsp/LockFreeRingBuffer.h` | C4324 alignment padding (pre-existing) |
| 3 | `src/gui/theme/AzTheme.cpp` | C4459 `text` shadowing (pre-existing, same pattern as the one fixed) |
| 1 | `src/app/MainComponent.cpp:2454` | C4996 `juce::Displays::Display::userArea` (pre-existing; window-sizing code, not a lane-M path) |

Lane-M files with **zero** warnings: `SoundcheckController.{h,cpp}`,
`SoundcheckPanel.{h,cpp}`, `SoundcheckSignal.*`, `SoundcheckCandidates.*`,
`LoopGainEstimator.*`, `AudioEngine.{h,cpp}`, and every lane-M region of
`MainComponent.cpp`. `HandsFreeTests.vcxproj` produced zero warnings.

The `SoundcheckPanel.cpp(302) warning C4459` the verifier reported is **absent**.

### Full suite

```
$ cd build && ctest -C Release
...
100% tests passed, 0 tests failed out of 714

Total Test time (real) =  62.41 sec
```

**714 = 712 (base) + 2 new.** Re-run after the mutation pass was reverted and
after the `AudioEngine.h` comment landed: 714/714 both times (63.36 s).

---

## 3. Mutation results

Three mutations applied together, test target rebuilt once, the three tests run.

| # | mutation | test | result |
|---|---|---|---|
| I-1 | drop `+ juce::String::fromUTF8 (kScConfirmDetectionOff)` from `soundcheckConfirmText` | `TheConfirmationSaysHaMasterTruocAndTheRealDuration` | **FAILED** |
| I-2 | preflight back to `notchControllers_[displayedSlot_]->copySnapshot (risk)` | `RingRiskOnAnUndisplayedSlotRefusesTheRun` | **FAILED** |
| I-3 | `showWorseOff()` → `worseOff()` in `rebuildSummary` (both sites) | `AWorseOffSlotIsNotMaskedByAnotherSlotsSuccess` | **FAILED** |

```
[  FAILED  ] SoundcheckPanel.AWorseOffSlotIsNotMaskedByAnotherSlotsSuccess
  test_gui_wiring.cpp(2136): Value of: panel.hasErrorForTest()
    Actual: false  Expected: true
  test_gui_wiring.cpp(2143): Value of: text.containsIgnoreCase ("KÉM an toàn hơn")
    Actual: false  Expected: true
  test_gui_wiring.cpp(2148): Value of: text.contains ("3")
    Actual: false  Expected: true

[  FAILED  ] MainComponentSoundcheck.TheConfirmationSaysHaMasterTruocAndTheRealDuration
  test_gui_wiring.cpp(2643): Value of: text.contains ("bộ chống hú TẮT trên mọi slot")
    Actual: false  Expected: true
  test_gui_wiring.cpp(2646): Value of: text.contains ("DỪNG")
    Actual: false  Expected: true

[  FAILED  ] MainComponentSoundcheck.RingRiskOnAnUndisplayedSlotRefusesTheRun
  test_gui_wiring.cpp(3096): Value of: confirmAsked
    Actual: true   Expected: false
  test_gui_wiring.cpp(3102): Value of: app.lastMessageForTest().contains ("nguy cơ hú")
    Actual: false  Expected: true
  test_gui_wiring.cpp(3105): Value of: measureButton.isEnabled()
    Actual: false  Expected: true

[  PASSED  ] 0 tests.
[  FAILED  ] 3 tests
```

The I-2 mutation is worth reading twice: `confirmAsked` came back **true** and
`measureButton` **disabled** — i.e. with the old gate the run reached the
confirmation dialog and sat there with DO dead, in a room that was ringing.

Sources restored and verified byte-identical with `cmp` before the rebuild.

### Peakiness measurement (M-1)

Measured with a temporary `std::printf` in the two flake guards, on a throwaway
incremental build; the instrumented file was restored with `cp` and verified
with `cmp` before the clean build.

```
--- Box-Muller (shipped) ---
MEASURE quiet worstPeakiness=1.450228
MEASURE gate=5.0 worstPeakiness=9.509144
MEASURE gate=20.0 worstPeakiness=9.509144

--- std::normal_distribution (previous, for comparison only) ---
MEASURE quiet worstPeakiness=1.431825
MEASURE gate=5.0 worstPeakiness=9.753669
MEASURE gate=20.0 worstPeakiness=9.753669
```

---

## 4. Commits

| SHA | scope |
|---|---|
| `0bb5bda` | `fix(soundcheck): final-review fixes I-1/I-2/I-3 + M-2/M-3/M-6 + C4459` — production + covering tests |
| `94489a6` | `test(soundcheck): portable noise fixtures (M-1), M-5 comment, M-7 fixture` |
| *(this one)* | `docs(soundcheck): detection-off + worst-slot ring-risk gate; spec line numbers; fix-wave report` |

`.superpowers/sdd/.gitignore` removed before each commit (repo policy: the SDD
workspace is TRACKED; the skill's script rewrites that file on every run).
No `git stash`, no `git add .` — explicit paths only.

---

## 5. What I could not do, and what a human still owns

1. **`applySoundcheckProposals()`'s per-slot accumulation is not covered by a
   headless test.** Reaching it needs `State::Results`, which needs a real
   device and ≥ 4.5 s per output with blocks driven throughout — the same
   limitation `AnAppliedReportHoldsTheConsoleAndADismissedOneDoesNot` already
   records, and the same reason the tester notes warn that the first real
   APPLY happens on someone's rig. The I-3 test pins the contract *around* the
   loop: two real `SoundcheckApplyStats`, summed exactly as the owner sums them,
   the per-slot predicate applied before the sum destroys it, and the strip's
   answer. The three-line loop body itself is verified by reading, not by
   running.
2. **No listen on a real rig.** This wave touched the audio callback in exactly
   one place, and only to add braces (M-6); nothing else in it changes a sample.
   The one user-visible behaviour change on a PA is that `ĐO` now **refuses more
   often** — a slot the console is not showing can block a run. That is the safe
   direction, but a soundman who sees `LOW` on screen and a refusal in the
   message bar will call it a bug unless they have read tester note 8, which
   now says so.
3. **Expected level change: none.** No gain, coefficient, clamp, ladder or
   buffer figure moved. No clamp, NaN guard or bounds check was removed or
   weakened; M-3 adds one.
4. **`AzTheme.cpp` still carries three C4459 `text` shadowings** of exactly the
   kind fixed in `SoundcheckPanel.cpp`. Out of lane M's scope, left alone, worth
   a separate sweep.
5. **The `ring_risk_slot` field is new in `soundcheck_start`.** `tools/logstats.py`
   does not read it and does not need to; anything downstream that pins the
   event's field set should be told.
6. **Release gate unchanged.** 1.3.0 is still blocked on the owner's approval of
   the 9 + 6 items recorded in `KY-THUAT-CHONG-HU.md` and the decision log. This
   wave changed nothing about that list.
