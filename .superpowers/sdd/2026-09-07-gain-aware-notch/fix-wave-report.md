# Lane G — fix wave after the final whole-branch review

**Baseline:** `af032f2` · **commits:** `63c1759` (code) → `29906b8` (docs) · **suite:** 547/547.

One wave, two commits. Every finding below is either FIXED with the file and
line, or explained where the ruling was amended rather than applied verbatim.

---

## I-1 (docs) — the "deeper than the DEPTH slider = bug" criterion was false for Preset/Manual/Soundcheck

The tester-facing bug criterion told the alpha team to report **any** notch
sitting deeper than the DEPTH slider. That is only true for Detector notches:
Preset/Manual/Soundcheck carry their own ceiling (Q8), so a preset saved after a
howl reloads at `deepestDb` — up to −24 dB — with the slider at −6. That is the
exact flow `TESTER-NOTES.md` step 3 asks the testers to run, so the docs were
asking them to report the thing the docs were asking them to do.

**Fixed:**

- `installer/TESTER-NOTES.md:85-94` — the criterion is now qualified to a notch
  **của detector** (the row with a LANE column in ACTIVE NOTCHES, explicitly not
  Preset/Manual/Soundcheck), plus a new paragraph saying Preset/Manual/Soundcheck
  notches carry their own ceiling and **are allowed** to sit deeper, naming the
  step-3 flow that produces it.
- `docs/release-notes/1.2.0-alpha.md:126-131` — same qualification in the
  "Cần báo lại ngay" list, same explicit Preset/Manual/Soundcheck sentence.

**Grepped for the same claim elsewhere** (`grep -rn "sâu hơn" installer/TESTER-NOTES.md
docs/release-notes/1.2.0-alpha.md docs/GIOI-THIEU.md docs/KY-THUAT-CHONG-HU.md`):
13 hits, only the two above are the bug criterion. The rest are the
shallower-ceiling warning (TESTER-NOTES:75, release-notes:112/115,
GIOI-THIEU:51, KY-THUAT:430/433/434), the −24 clamp (release-notes:43,
KY-THUAT:409/463), the room-memory deepen-only rule (GIOI-THIEU:46,
KY-THUAT:270) and the risk table (KY-THUAT:465) — all correct as written and
all already naming Preset/Manual/Soundcheck as exempt from the ceiling pull.
`docs/GIOI-THIEU.md` and `docs/KY-THUAT-CHONG-HU.md` carry no bug-report list at
all, so nothing to qualify there.

## I-2 (spec + plan) — invariant 3 claimed ≤ 6 dB in the deep direction; a RECLAMP is up to 18 dB by design

`NotchController.cpp:1372` computes the reclamp target as
`std::max (n.deepestDb, ceilingDbFor (n))` and pushes it in one 10 ms ramp. From
−6 (fully released) back to −24 that is **18 dB in the deep direction in one
step**, which §4.4 intends and which invariant 3 flatly denied.

**Fixed:**

- `docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md` §4.10
  invariant 3 — rewritten: a **DEEPEN** step is ≤ 6 dB (and can be smaller at an
  off-rung ceiling, Q13); the shallow direction is **unbounded**; a **RECLAMP**
  is a deliberate exception of up to 18 dB, justified because the target is a
  depth the notch already held, invariant 2 still holds, `|H| ≤ 1` at every ramp
  midpoint (`Biquad.RampMidpointsNeverBoostAnyFrequency`), and the coefficient
  walk is continuous so there is no reset and no click. Rate stated as ≤ 0.6
  dB/ms for a deepen and ≤ 1.8 dB/ms for a worst-case reclamp, and the paragraph
  now says **`NotchChain` enforces none of it — the ladder does**, and only while
  the ladder is the sole caller of `pushRetuneLocked`.
- `docs/superpowers/plans/2026-09-07-gain-aware-notch.md` Global Constraints —
  the "A deepening step is at most 6 dB" bullet is now "A **DEEPEN** step…"
  with an unbounded shallow direction, a new "A RECLAMP is the deliberate
  exception, up to 18 dB" bullet, and the ramp bullet carries the
  NotchChain-enforces-nothing sentence.

**Also fixed (§4.6, the second half of I-2):** the spec said memory is written
"khi Clear do `AutoRelease` từ bậc −6". The write actually sits in the `else` of
`if (n.depthDB < kDepthLadderDb[0])` (`NotchController.cpp:830-844`), i.e. every
notch with `depthDB >= −6` when the quiet clock expires — which includes an
off-rung Manual −3 dB, and records `deepestDb = −3`. The bullet now says exactly
that: what is recorded is "the depth this bin needed", not "the −6 rung".

## I-3 (SDD workspace untracked)

Out of this wave's scope by instruction ("never stage anything under
`.superpowers/`"). The workspace, including this report, is left for the
controller to commit at closeout. `.superpowers/sdd/.gitignore` was deleted
before each of the two commits below (repo policy, memory note
`sdd-workspace-gitignore-trap-2026-09-04`); `git status` confirmed the only
untracked path either time was `.superpowers/sdd/2026-09-07-gain-aware-notch/`.

## I-4 (picture) — the off-list ceiling render had no image

`fba2626` changed what the tuning strip DISPLAYS for a ceiling that sits on no
combo rung (shown as text instead of leaving the combo blank, and
`currentParams()` reports it back rather than snapping to the first item). No
image showed it.

**Fixed:** `tools/snapshot.cpp` renders a **third** shot,
`console-preset-music.png`, after `console-live.png` and from the same frame —
same three staged ages, same verdicts, same chip. It sets `setNotchDefaults
(25.0, -10.0)` (`presets/Music.json`'s pair, verified against the file) on every
controller via `getNotchControllerForTest(i)` for `i < kMaxSlots`, then re-reads
both panels through their providers: `app.getTuningPanel().refresh()` (reads
`paramsProvider`, which reads controller 0) and `slots.refresh()` (reads
`slotTuningProvider` per row), then `app.resized()`. The defaults go straight to
the controllers rather than through `loadPreset()`, which would open a file
chooser and drive the device restart cycle. `console-live.png` is produced by
untouched code.

The header comment block at the top of the file lists the third output.

### What the PNG shows

Read back with the Read tool, then re-cropped to the tuning strip at 2× and read
again:

- **DEPTH** cell reads **`-10 dB`** — as text, with the combo's dropdown arrow
  still drawn, not blank.
- **Q** cell reads **`25`** — likewise.
- The rest of the strip is unchanged and correct: RISE `250 ms`, HOLD `3`,
  THR `10.0`.
- The RESPONSE badge reads **CUSTOM**, which is right: an off-list pair matches
  none of the three curated presets, so `updatePresetFor` falls through to
  `kPresetCustomId`.
- Everything else matches `console-live.png`: three notches (247 Hz L −18.0 dB
  GOOD, 1.9 kHz L −6.0 dB FALSE, 1.2 kHz R −12.0 dB with both verdict buttons),
  the dashed R stem, LINK on slot 02 / INDEP on slot 01, RING RISK CRITICAL
  (staged, as the tool prints on stdout).

`shots/` is **not** committed.

## M-1 (code) — `stageChangedAtMs` stamped at four call sites, never inside `pushRetuneLocked`

**Expected level change: 0 dB.** Behaviour is identical; this moves bookkeeping,
not arithmetic. No clamp, no lock order, no DSP path touched.

**Done:**

- `src/app/NotchController.cpp:331-341` — `pushRetuneLocked` now writes
  `n.stageChangedAtMs = liveMs_` beside `n.depthDB = newDepthDb`, on the success
  path only (a refused retune still touches nothing).
- Three external stamps **deleted**: the Ceiling site
  (`:797-803`), the Release site (`:822-827`), the Deepen site (`:1443-1451`).
  Each is now guarded solely by `if (pushRetuneLocked (...))`, so the behaviour
  is bit-identical.
- **One external stamp kept, deliberately** — the Reclamp site
  (`:1379-1400`). That branch is entered on the short-circuit
  `target == n.depthDB || pushRetuneLocked (...)`, i.e. there is a path that
  reclamps **without pushing anything** (the notch already stands at the
  target). That path must still re-arm the DEEPEN gate — the notch WAS
  reclamped, it merely had nowhere to go. Deleting this fourth stamp would have
  changed behaviour, which the instruction ruled out ("Behaviour identical"). In
  the pushing path it is a no-op writing the same `liveMs_`. The comment there
  says so.
- `src/app/NotchController.h:479-482` — the `stageChangedAtMs` field comment now
  says who stamps it and names the one exception.

### RED

New test `NotchControllerLadder.RetuneStampsStageChangedAtMs`
(`tests/test_notchcontroller.cpp:2088-2129`). It places a Detector notch, runs
500 ms of live time through `runOnce()`, calls `retuneForTest` (which drives
`pushRetuneLocked` directly, so it observes the **helper's** contract, not one
caller's version of it), and asserts `stageChangedAtMsForTest == liveMsForTest`.
It also asserts a REFUSED retune (−40 dB, past the ladder floor) leaves the
stamp alone.

Proved red by temporarily removing the new line from `pushRetuneLocked`,
rebuilding `HandsFreeTests` and running the single test:

```
[ RUN      ] NotchControllerLadder.RetuneStampsStageChangedAtMs
tests\test_notchcontroller.cpp(2114): error: Expected equality of these values:
  h.controller.stageChangedAtMsForTest (0, 1)
    Which is: 0
  h.controller.liveMsForTest()
    Which is: 500
a successful retune must stamp stageChangedAtMs with the CURRENT live time, from inside pushRetuneLocked

tests\test_notchcontroller.cpp(2117): error: Expected: (h.controller.stageChangedAtMsForTest (0, 1)) > (atPlacement), actual: 0 vs 0

[  FAILED  ] NotchControllerLadder.RetuneStampsStageChangedAtMs (3 ms)
[  PASSED  ] 0 tests.
[  FAILED  ] 1 test, listed below:
[  FAILED  ] NotchControllerLadder.RetuneStampsStageChangedAtMs

 1 FAILED TEST
```

The line was then restored (verified: `grep -n "RED-DEMO" src/app/NotchController.cpp`
returns nothing).

### GREEN

```
cmake --build build --config Release
```

→ built clean; `HandsFreeSnapshot.exe`, `HandsFreeTests.exe`, `PeakinessSweep.exe`
all relinked.

Focused:

```
./build/tests/Release/HandsFreeTests.exe --gtest_filter='NotchController*'
[----------] 56 tests from NotchControllerLadder (4231 ms total)
[==========] 123 tests from 14 test suites ran. (5961 ms total)
[  PASSED  ] 123 tests.
```

Full suite:

```
cd build && ctest -C Release
547/547 Test #547: logstats_fixture .............................................................................   Passed    0.10 sec

100% tests passed, 0 tests failed out of 547

Total Test time (real) =  43.24 sec
```

547 = 546 + the one new test, as predicted.

## M-2 (docs) — the release window omitted the −6 case

- `installer/TESTER-NOTES.md:127-128` (walkthrough step 5) — "40–60 giây tổng
  cộng" → "**30–60 giây** tổng cộng, tùy bậc nó đang đứng khi hết hú: −6 → 30 s;
  −12 hoặc −10 → 40 s; −18 → 50 s; −24 → 60 s".
- `docs/KY-THUAT-CHONG-HU.md:251-255` — the ladder bullet now reads "**30–60 s**
  tùy bậc đang đứng lúc bin im: 30 s từ −6 (đã ở bậc nông nhất, hết 30 s là
  `Clear` luôn, không có bậc nào để nhả), 40 s từ −12 (và từ −10 của
  `Music.json`: `nextShallowerRungDb(−10) = −6`), 50 s từ −18, 60 s từ −24".

**Two more the finding did not name**, found by `grep -rn "40–60\|40-60" docs/
installer/ memory/` — the plan's own instructions to Task 10 still specified the
40–60 s wording, which would have made a future session "fix" the docs back:
`docs/superpowers/plans/2026-09-07-gain-aware-notch.md:4832` and `:4840`, both
corrected to the 30–60 s window with the per-rung breakdown.

Already correct and left alone: `docs/GIOI-THIEU.md:45`,
`installer/TESTER-NOTES.md:54-60`, `docs/release-notes/1.2.0-alpha.md:26-27`
and `:95-96` — earlier rounds had already fixed those.

## M-8 (docs) — two wrong citations

- `docs/KY-THUAT-CHONG-HU.md:121-125` — the measured-attenuation run was cited
  against `af5201e` → now `fba2626`, plus a sentence explaining the number does
  not depend on any lane G commit: **`Biquad` and `NotchChain` are unchanged
  since `cf023eb`**, and `cf023eb` itself only edited a comment in
  `NotchChain.h`. Verified:

  ```
  git log --oneline cf023eb..HEAD -- src/dsp/Biquad.h src/dsp/Biquad.cpp src/dsp/NotchChain.h src/dsp/NotchChain.cpp
  (no output)
  ```

- `docs/KY-THUAT-CHONG-HU.md:429-431` — cited `NotchController.cpp:786-796` for
  the ceiling branch. Re-grepped **after** the M-1 edit (which added 8 lines
  above it): the branch `if (n.depthDB < ceiling)` is now at `:792`, closing at
  `:805`, inside `if (n.origin == Origin::Detector)` which opens at `:752`. The
  doc now names the branch condition as well as the range, so the next reader
  can find it even if the numbers move again.

## M-9 (plan) — checkboxes

`docs/superpowers/plans/2026-09-07-gain-aware-notch.md`: 73 `- [ ]` boxes, 0
ticked. Ticked **72** — every step of Tasks 1–10 including Task 10 Step 9
(snapshot, done). Task 10 Step 10 (`:4882`, release) left unticked and annotated
`— **chưa xong (release-alpha.ps1 chạy sau fix wave này)**`. Verified: exactly 1
`- [ ]` remains.

## Test count and SHA refresh

`grep -rn "546/546" docs/ installer/ memory/ README.md` before the wave:

```
docs/KY-THUAT-CHONG-HU.md:5
docs/KY-THUAT-CHONG-HU.md:527
docs/release-notes/1.2.0-alpha.md:3
docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md:70
docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md:5
installer/TESTER-NOTES.md:3
```

`grep -rn "fba2626" ...` before the wave — the same six, plus
`docs/KY-THUAT-CHONG-HU.md:427` (historical: names the fix round the warning
came from) and `memory/gain-aware-notch-lane-g-2026-09-07.md:4` (the commit
range).

All six status statements updated to **547/547 at `63c1759`**, and the commit
range `01ecb4c..fba2626` → `01ecb4c..63c1759` in the roadmap and the memory note.
The "+92 so với 1.1.3 (454)" figure in KY-THUAT §8 became +93.

After the wave:

```
grep -rn "546" docs/ installer/ memory/ README.md
docs/superpowers/plans/2026-08-23-gui-console-redesign.md:40   (hex colour #546E7A)
docs/superpowers/specs/2026-08-23-gui-console-redesign-design.md:24   (same)

grep -rn "fba2626" docs/ installer/ memory/ README.md
docs/KY-THUAT-CHONG-HU.md:122   (historical: which binary the attenuation was measured on)
docs/KY-THUAT-CHONG-HU.md:433   (historical: which fix round the warning came from)

grep -rn "af5201e" docs/ installer/ memory/ README.md
docs/superpowers/specs/2026-09-05-data-loop-design.md:125   (historical: log format boundary)
memory/gain-aware-notch-lane-g-2026-09-07.md:82             (historical: same)
memory/gain-aware-notch-lane-g-2026-09-07.md:159            (historical: --expect-retunes)
```

Zero non-historical hits remain.

## Extra: statements this wave made false (repo rule 12)

`grep -rn "console-live\|console-idle"` found three places claiming the snapshot
tool writes exactly two images:

- `README.md:78` — now names all three and says what the third one shows.
- `.claude/skills/juce-component-snapshot/SKILL.md:31-36` — same.
- `docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md` §5.4 — said
  "`HandsFreeSnapshot` không cần stage mới", which stopped being true twice
  (once at `97521aa`, once here). Rewritten to list both shots and what each is
  evidence for.

`CLAUDE.md:111-112` ("Send `shots/console-live.png` and `console-idle.png` when
the empty state changed") is not false, only incomplete — it is a standing owner
instruction, so it is left for the owner rather than edited here. Flagged under
Concerns.

## memory/

`memory/gain-aware-notch-lane-g-2026-09-07.md` gains **lesson 21** — bookkeeping
that belongs to a write must live beside the write, not at the call sites; four
correct copies keep every test green and the danger is the fifth caller. It also
records the trap in the obvious cleanup: one of the four call sites has a
no-push short-circuit and deleting its stamp changes behaviour. The note is
already indexed in `memory/MEMORY.md:26`.

## Concerns / left for a human

1. **The reclamp stamp is asymmetric on purpose.** Three call sites lost their
   stamp, one kept it. Anyone tidying that fourth one away later will silently
   change the DEEPEN gate on the no-push reclamp path. The comment and memory
   lesson 21 both say so, but there is no test pinning it — a test would have to
   contrive `target == n.depthDB` at a reclamp.
2. **`CLAUDE.md`'s snapshot instruction now lists two of three images.** Owner's
   file, owner's call.
3. **`shots/console-live.png` on disk was re-rendered with `--fast`**, so its
   three notch ages read 0s rather than the aged 22s/10s/1s of the image
   previously sent to the owner. The code producing it is unchanged; a slow run
   reproduces the aged version. `shots/` is not committed either way.
4. **Not run:** `installer\release-alpha.ps1`, per instruction. The branch is
   still unpackaged, and every status line says so.
5. **Untouched out-of-scope drift** the final review listed: `README.md:10`
   still says 1.0.3, and `docs/superpowers/plans/2026-09-05-data-loop.md:1728`
   still has the stale `ev` table.
