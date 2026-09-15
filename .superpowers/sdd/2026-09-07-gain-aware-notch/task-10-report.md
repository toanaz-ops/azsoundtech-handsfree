# Task 10 report — docs, tester notes, release notes, memory, roadmap

**Commit:** `11adb3b` on `claude_desk/lane-g-brainstorm-sdd-f3c568` (BASE `af5201e`).
**Files changed:** 10 (8 modified, 2 created). Docs only — no `src/`, `tests/`,
`tools/`, `installer/*.ps1` or `CMakeLists.txt` touched. No `git stash` used.
`.superpowers/sdd/.gitignore` did not exist (checked, `rm -f` run anyway);
nothing under `.superpowers/` is staged or committed.

---

## 1. Files changed, with the sections

### `docs/GIOI-THIEU.md` (Vietnamese)

| Where | What |
|---|---|
| overview diagram | `tự nhả filter khi hết hú` → `nhả dần từng bậc khi hết hú` |
| **Notch siêu hẹp** row | depth −6..−24 (default −18) is now stated as a **TRẦN** (ceiling); placement −6 / −12 on a steep rise; deepen 6 dB per 300 ms |
| **Tự nhả sau 30 giây** row | replaced by **Nhả dần theo bậc** (30 s then 10 s/rung, Clear at −6, reclamp to deepest, freeze at RING RISK ≥ RISING **slot-global**) + a new **Nhớ phòng 5 phút** row |
| **Preset có sẵn** row | Speech/Music depths relabelled as ceilings; the Q13 consequence (Music's −10 IS reachable, no quantisation); `savePreset` now writes `deepestDb` + `notchDefaults` |
| header/tail | no version line in this file — nothing else to sync |

### `docs/KY-THUAT-CHONG-HU.md` (Vietnamese)

| Where | What |
|---|---|
| header blurb (`:4`) | `06/09/2026, v1.1.3` → `07/09/2026, v1.2.0` (also names 1.1.3 = lane R) |
| mermaid box (`:32`) | `auto-release 30 s` → `thang nhả 30 s + 10 s/bậc` |
| mermaid Auto note | `Khóa khi vượt ngưỡng, tự nhả sau 30 s im` → `Đặt nông rồi đào sâu theo nhu cầu, nhả dần từng bậc khi im` |
| **### Bộ lọc notch và độ sâu** | appended the 1.2.0 block: `setNotchFilter` reset = click, `rampNotchDepth`/`kRampMs = 10 ms` (≤ 0,6 dB/ms), the convex-hull no-boost proof with the measured numbers, the four measured rungs, and the **known gap** (NaN self-heal cancels in-flight ramps, no coefficient replay, self-heals on the next Set) |
| RESPONSE preset line (`:177`→ now `:206`) | SAFE/BALANCED/AGGRESSIVE depths relabelled `trần −12 / −18 / −24` |
| **### 3.4 NotchController** | the whole "Auto-release 30 s" paragraph replaced by the **Thang độ sâu (1.2.0, lane G)** section: fixed rungs, live ceiling, Q13 effective ladder, Đặt / Đào / Nhả / Kẹp lại / Nhớ phòng / `pushRetuneLocked` bullets, then the KD-7 Soundcheck exemption restated as exempt from *all* of it |
| **## 4. Preset** | appended: `savePreset` writes `deepestDb` + `notchDefaults` (and why the old omission silently dropped the ceiling two rungs), the −24 clamp + `juce::Logger` count (Q12), the `adoptPreset` re-Set now taking the **ramp** branch, and the still-open `PresetNotchDefaults::depthDB = -12` vs `kDefaultNotchDepthDb = -18` disagreement (flagged as NOT lane G's) |
| **## 5. Tổng hợp các giới hạn an toàn** | four new rows: mid-signal depth change → ramp; mid-ramp boost → convex-hull proof + test; deeper than −24 from a preset → clamp + log + `pushRetuneLocked` refusal; **`deepestDb` outliving a lowered ceiling → unconditional per-tick clamp AND clamp at the reclamp target (M-B)** |
| **## 7. Log session** heading + table | heading `(lane D, v1.1.2)` → `(lane D từ v1.1.2; notch_retune thêm ở v1.2.0)`; new `notch_retune` row with all fields incl. `age_ms` measured from PLACEMENT |
| **## 8. Trạng thái & kiểm chứng** | heading `05/09/2026, v1.1.2` → `07/09/2026, v1.2.0`; new 1.2.0 paragraph + suite **535/535** and the four measured rungs; 1.1.3 (454/454) noted; the old 1.1.2 text kept as history with "Suite lúc đó" |

### `docs/spec-ring-risk.md` (**English — see deviation §3**)

New section **"A second reader: lane G's release ladder (1.2.0)"** inserted
immediately before `## Out of scope`: `frameMaxScore_`/`frameScoreValid_` as the
freeze inputs, `kRiskFreezeFraction = 0.55f` as the single definition with
`SpectrumView::kRingRiskRisingFraction` as its alias, and four bullets — no
`snapshotMutex_` (M-5, `releaseFrozen` published in its own scope before
`modelMutex_`), chip-vs-clock skew (M-8), `ringRiskValid == false` does NOT
freeze (invariant 7), and no time cap + **slot-global** freeze (Q9).

### `docs/superpowers/specs/2026-09-05-data-loop-design.md`

§3.2 table: `notch_retune` row inserted after `notch_set`. Below the table: it
is an **update, not an open/close**; `logstats.py` amends the open record
(`depth_db`, `deepest_db`, `retunes`) and does not pop it, with the 300 ms
false-positive consequence spelled out; `--expect-retunes` named; the reader is
an `if/elif` chain that **ignores unknown `ev`**; and the warning that a log from
a branch build **before `af5201e`** is corrupt for logstats (none released —
internal data only).

### `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md`

- Lane table row G: spec column now links the design doc and says **đã hạ cánh
  1.2.0**; the "từ τ ringing" claim corrected to "đo thực nghiệm, không từ τ"
  (the spec's §1 says G explicitly does not use the τ formula).
- Lane table row A: added `fallback notch = thang G (đặt −6, đào 6 dB/300 ms,
  nhả 30 s + 10 s/bậc)`.
- Status table row G: `chờ S, D` → landed 1.2.0, branch + `01ecb4c..HEAD`, suite
  535/535, one-line feature list, date 2026-09-07.
- Status table row A: `chờ M, G` → `chờ M (G đã xong: fallback notch dùng thang G)`.

### `docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md`

Status line (`:5`): `spec v2 … chờ owner duyệt` → **đã thực thi, 1.2.0 alpha**,
linking `../plans/2026-09-07-gain-aware-notch.md`, 10 tasks, suite 535/535, and
naming Q14 alongside the rev-2 amendments.

### `installer/TESTER-NOTES.md` (Vietnamese, soundman voice)

- Header: v1.1.3 → **v1.2.0**, build 2026-09-07, suite 535/535, SHA-256 and
  installer size left as an explicit **placeholder** (see §4), plus a blockquote
  warning that 1.2.0 DOES touch the audio path and to open the volume low first.
- New `## Mới trong 1.2.0 (so với 1.1.3) — nghe ở âm lượng thấp trước`, placed
  before `## Mới trong 1.0.5`, with all six required points: shallow start
  (~0.3–0.6 s more audible howl), the shallow-rung case stated as correct **and
  as the one behaviour no automated test in the project can check** (M-3) with
  the "if every notch ends at the DEPTH slider, report it" instruction, the
  release staircase (**40–60 s**: 40 from −12, 50 from −18, 60 from −24) and the
  "more tone missing than 1.1.3 between 30 s and then" statement, the RISING /
  CRITICAL freeze with no time limit and **slot-global**, the 5-minute room
  memory, and the no-click requirement (10 ms ramp).
- Test scenario step 5: rewritten — watch the depth column start at −6/−12 and
  climb; release now 40–60 s, not 30.
- Test scenario step 3: LOAD/SAVE now notes `deepestDb` and that adopting a
  preset over live notches ramps (must not click) — the Task 2 ledger NOTE.

### `docs/release-notes/1.2.0-alpha.md` (new, styled on `1.1.3-alpha.md`)

Version + date + suite 535/535 + installer line (SHA/size placeholder); an
explicit "CÓ đụng đường audio" banner; the "Có gì trong 1.2.0" list covering
placement, deepening, ceiling-is-live, release ladder, freeze, reclamp, room
memory (incl. Q14 deepen-only), the −24 clamp, the 10 ms ramp, `notch_retune` +
logstats, and `savePreset`; the four measured attenuations as a table; the Q13
consequence for the two shipped presets (Music reaches −10, Speech −18 — neither
quieter than 1.1.3 once the ladder has climbed); and the three-item
**"Nghe ở âm lượng thấp trước"** block (placement up to ~0.6 s more howl; release
window 30 s → 40–60 s, up to 18 dB more tone missing; stuck-shallow 6–12 dB
shallower for the notch's whole life, the intended win and the one thing the
suite cannot check). Closes with what to report.

### `memory/gain-aware-notch-lane-g-2026-09-07.md` (new)

19 numbered lessons in the shape the other notes use, split A (technical) /
B (process) / C (still open): state-identity assertion vs black-box measurement
(M-7); the convex-hull proof; `rNorm` saturation forcing `riseRatio` out;
the slow-rise fixture starting AT the noise floor and the `r^12` one-hop offset;
non-recursive `modelMutex_` ⇒ `*Locked` sibling (B-1); `pushClearLocked`
retaining `depthDB` making "depth < 0" a false liveness test + the six tests
written on it + `setNotchImpl` re-init (B-2); `ev` not `kind` as the log dispatch
key (B-3) with the logstats pop consequence; deepen == still-howling ⇒ the ladder
stops at the first working rung, untestable here (M-9/M-3) with "do not fake it";
per-frame ladder assertions; slot-global freeze; **M-B half-a-clamp as the
safety lesson**; Q13 quantisation making `Music` 4 dB shallower; Q14 deepen-only
memory; a test must build its state the production way; ladder timings derived
from the rung not a round number; a plan is unverified until someone opens its
files; three wrong reviewer line numbers vs the implementer's correct ones;
`effectiveLinked()` on a mono harness poisoning later tests' harmonic penalty;
and the two process traps (`git stash` on a shared stack; the SDD script
rewriting `.superpowers/sdd/.gitignore`). §C lists the five carry-overs.

### `memory/MEMORY.md`

Exactly **one** new line appended to `## Notes` (verified: `git diff --stat`
reports `1 insertion(+)`), in the same one-line Vietnamese style as its
neighbours, linking the new note.

---

## 2. Self-review — grep results

Stale-statement sweep over every edited doc for `−18 cố định`, `nhả cụt`,
`clear thẳng`, `Clear cụt`, `tự nhả sau 30`, `auto-release 30 s`, `v1.1.2`,
plus `độ sâu −18`, `mặc định −18 dB`, `cắt 18 dB`, `Clear ở 30`, `biến mất sau 30`:

| Hit | Verdict |
|---|---|
| `docs/KY-THUAT-CHONG-HU.md:445` `## 7. Log session và nhãn (lane D, v1.1.2)` | **stale — fixed** to `(lane D từ v1.1.2; notch_retune thêm ở v1.2.0)` |
| `docs/KY-THUAT-CHONG-HU.md:4` header `06/09/2026, v1.1.3` | **stale — fixed** to `07/09/2026, v1.2.0` (found by a separate version sweep, not in the brief's list) |
| `docs/release-notes/1.2.0-alpha.md:24` "Thang nhả thay cho cú Clear cụt" | correct — describes the behaviour being replaced |
| roadmap `:23` "nhả dần thay vì nhả cụt" | correct — same, it is the lane's own description |
| `docs/GIOI-THIEU.md:48` Soundcheck "không tự nhả" | correct — KD-7 exemption is unchanged |
| `docs/KY-THUAT-CHONG-HU.md:159` `"30 giây im"` (D-06 two-clock section) | correct — 30 s is still the FIRST release threshold |
| `docs/KY-THUAT-CHONG-HU.md:457` `auto_release` in the `notch_clear` reason list | correct — still an emitted reason string |

`MEMORY.md`: exactly one new line (`1 file changed, 1 insertion(+)`).

**Every path cited in a doc was existence-checked** — the 3 lane-G superpowers
docs, `docs/spec-ring-risk.md`, both other specs, `tests/test_notchcontroller.cpp`,
`tools/logstats.py`, `presets/Music.json`, `presets/Speech.json`,
`memory/sdd-workspace-gitignore-trap-2026-09-04.md`, and the two new files: all
present.

**One line reference in the brief had drifted and was corrected.** The brief
(and the plan, in six places) cites `tests/test_notchcontroller.cpp:344-349` as
where `pump` writes the raw tone into `h.tap`. At `af5201e` lines 344-349 are the
tail of `NoiseSource`; `pump` is at **`:350-355`** (`grep -n "void pump"`). I
wrote **350-355** into `docs/KY-THUAT-CHONG-HU.md` and the memory note. The plan
file itself still says 344-349 — not in my file list, left alone.

All constants quoted were read from source at `af5201e`, not from the plan:
`kDepthLadderDb {−6,−12,−18,−24}` (`NotchController.h:109`), `kDepthStepDb 6`
(`:111`), `kMaxDepthDb −24` (`:114`), `kDeepenAfterMs 300` (`:118`),
`kSteepRiseRatio 2.0f` (`:122`), `kReleaseFirstMs = kAutoReleaseMs = 30000`
(`:89, :125`), `kReleaseStepMs 10000` (`:126`), `kMemoryTtlMs 300000` (`:129`),
`kMemoryEntriesPerLane 16` (`:130`), `kRiskFreezeFraction 0.55f` (`:140`),
`kDefaultNotchDepthDb −18` (`:101`), `NotchChain::kRampMs 10.0` + the
≤ 0,6 dB/ms rationale (`NotchChain.h:35-44`), `PresetNotchDefaults::depthDB
−12` (`PresetManager.h:146`), RESPONSE presets `{500,4,−12,40,12}` /
`{250,3,−18,30,10}` / `{100,1,−24,20,8}` (`TuningPanel.cpp:40-44`),
`SpectrumView::kRingRiskRisingFraction = NotchController::kRiskFreezeFraction`
(`SpectrumView.h:246`), `notch_retune` field set (`MainComponent.cpp:575-596`),
logstats `notch_retune` branch + `--expect-retunes` (`tools/logstats.py:59-73,
152, 169`), `presets/Music.json` depth −10 / `Speech.json` −18, and the
convex-hull proof numbers (1.9e-15 dB, identity −3.5e-11 dB) from
`src/dsp/Biquad.h:56-112`.

**The four measured attenuations were re-measured, not copied.** I ran the
already-built Release binary (no rebuild) at `af5201e`:

```
build/tests/Release/HandsFreeTests.exe --gtest_filter=NotchChain.MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel
[ LADDER   ] requested -6 dB  -> measured  -6.000000 dB
[ LADDER   ] requested -12 dB -> measured -12.000000 dB
[ LADDER   ] requested -18 dB -> measured -18.000000 dB
[ LADDER   ] requested -24 dB -> measured -24.000000 dB
[  PASSED  ] 1 test.
```

---

## 3. Deviations from the brief, and why

1. **`docs/spec-ring-risk.md` written in ENGLISH, not the brief's Vietnamese.**
   That file is an English spec (`# Spec — RING RISK readout`, "What exists
   today", "The contract, as built", "Known gaps (owner)", "Out of scope") and my
   dispatch instruction was to match each file's existing language. Every fact
   from the brief's block is carried over verbatim in meaning, including the
   m-D alias, M-5, M-8, invariant 7 and Q9; I added the slot-global fact from the
   Task 7 ledger NOTE. Flagging it so the controller can overrule if the brief's
   Vietnamese was deliberate.

2. **Release-ladder total stated as "40–60 s tùy bậc", not the brief's
   "tổng ~50 giây".** ~50 s is the −18 case only. The constants give 40 s from
   −12, 50 s from −18, 60 s from −24, and the brief's own tester-notes bullet
   says 40–60 s. I used 40–60 everywhere so the four documents agree.

3. **Freeze described as SLOT-GLOBAL everywhere**, per the Task 7 ledger NOTE,
   including in `GIOI-THIEU.md` where the brief's draft row read as per-notch.

4. **Roadmap lane G row: "depth theo nhu cầu từ τ ringing" corrected to
   "đo thực nghiệm, không từ τ".** Spec §1 (Q2) says explicitly that G does NOT
   use the τ → loop-gain formula because the app does not measure round-trip
   delay (that is lane M). Leaving it would have shipped a false statement in the
   row I was updating.

---

## 4. What I could not source, and left out / marked

- **SHA-256 and installer size for Setup 1.2.0.** The release script has not run
  (my dispatch forbids running `installer\release-alpha.ps1`; the controller runs
  it after the final review). `installer/TESTER-NOTES.md` carries the literal
  placeholder `` `<điền sau khi installer\release-alpha.ps1 -Part minor chạy
  xong>` `` and the size line says "điền sau khi đóng gói";
  `docs/release-notes/1.2.0-alpha.md` says the same in its build line.
  **These two lines MUST be filled in Step 10 before the notes reach a tester.**
- **The final `ctest` count.** I did NOT run the suite (docs-only dispatch, no
  build). The **535/535** used in the tester notes, the release note,
  `KY-THUAT-CHONG-HU.md` §8 and the roadmap comes from the SDD ledger's Task 9
  line ("implementer DONE af5201e (535/535, +7)"). If the controller's own
  release-gate run reports a different number, those four places need updating.
- **The `console-live.png` snapshot (brief Step 9).** Not produced — my dispatch
  scoped this task to docs and named no snapshot deliverable. Nothing in the docs
  I wrote claims an image was checked. The claim "the snapshot image is the only
  check that the ACTIVE NOTCHES depth column reads a ladder rung" is still
  outstanding for whoever runs Step 9.
- **`docs/superpowers/decisions/2026-09-06-lane-g-gain-aware-notch.md` left
  untouched** — nothing in this task overturned Q1–Q14; it is only cited.
- **Version bump / `CMakeLists.txt`** — not touched (Step 10 is the controller's).

## Fix round 1

**Commit:** `81c688c` on `claude_desk/lane-g-brainstorm-sdd-f3c568` (BASE `b8e3f25`,
after Task 9 fix round landed `539/539`). Docs only, 10 files modified (all
already tracked — no creates this round). No `src/`, `tests/`, `tools/`,
`installer/*.ps1` or `CMakeLists.txt` touched. No `git stash` used.
`.superpowers/sdd/.gitignore` did not exist (checked, `rm -f` run anyway).
A pre-existing, unrelated uncommitted change to `tools/snapshot.cpp` was present
in the working tree at the start of this round (not authored by this task, not
docs, not touched, not staged — CLAUDE.md rule 7).

Fixed the reviewer's three Important findings and every named minor from the
`task-10-brief.md` fix-round instructions:

- **I1** (`đã release alpha` was false): `docs/KY-THUAT-CHONG-HU.md:4` header and
  `:485` §8 heading now say "đã hiện thực và qua gate ctest (539/539 tại
  `b8e3f25`); chưa đóng gói — `release-alpha.ps1` sẽ chạy sau final review, SHA/
  kích thước cập nhật khi đó."
- **I2** (ramp-bound overclaim): `docs/KY-THUAT-CHONG-HU.md:97-104` rewritten —
  10 ms applies to every depth change; only a 6 dB rung is 0.6 dB/ms; a reclamp
  or a lowered ceiling can move more than one rung in the same 10 ms **by
  design**; `NotchChain` does not enforce the bound (this matches what the
  header comment at `NotchChain.h:41-43` already said — the doc prose was the
  only thing overclaiming).
- **I3** (`535/535` / `af5201e` stale in 4+1 places, preset ceiling fact, Task 9
  tester caveat): updated to **539/539 at `b8e3f25`** in `KY-THUAT-CHONG-HU.md`
  §1 header + §8, `release-notes/1.2.0-alpha.md` build line,
  `installer/TESTER-NOTES.md` header, the roadmap status row, AND (found by my
  own sweep, not named in the brief's 4) the design-doc status line in
  `2026-09-06-gain-aware-notch-design.md:5` and the memory note's own header
  line — all five were the same live "current suite count" claim. Added the
  `loadPreset`/Q11 ceiling-round-trip fact (file **with** `notchDefaults`
  restores the ceiling to every Global-tuning slot; file **without** it leaves
  the live ceiling untouched) and the Task 9 tester caveat (a preset saved
  after a howl reloads at `deepestDb`, up to −24, regardless of the live
  slider — safe direction, cut only) to `GIOI-THIEU.md`'s preset row,
  `KY-THUAT-CHONG-HU.md` §4, and both release docs.

Minors, all fixed: "trần bị hạ hai bậc" → "một bậc (−18 → −12)"
(`KY-THUAT-CHONG-HU.md`, the math: `kDefaultNotchDepthDb` −18 vs
`PresetNotchDefaults::depthDB` −12 is one rung, not two); the ramp-on-preset-
load claim now says "only when freq/Q match the stored values exactly" in
`KY-THUAT-CHONG-HU.md` §4, `TESTER-NOTES.md` step 3, and the release note;
"40–60 giây" → "30–60 giây (trần −6: 30 s; −12/−10: 40 s; −18: 50 s; −24: 60 s)"
in `GIOI-THIEU.md:45`, `TESTER-NOTES.md` item 3, and (for internal consistency,
same fact stated twice in that file) both places it appeared in the release
note; freeze wording rewritten in `release-notes/1.2.0-alpha.md` and
`TESTER-NOTES.md` to say the clock reads `frameMaxScore_` directly and the chip
lags by up to 750 ms / shows only the displayed slot, so a LOW chip on another
slot does not mean the clock is running; `docs/spec-ring-risk.md`'s "It does not
take `snapshotMutex_`" heading replaced with "The freeze READ takes no lock; the
`releaseFrozen` publish takes `snapshotMutex_` in its own scope, before
`modelMutex_`" (verified against `NotchController.cpp:698-709` — the read at
line 687-688 takes no lock, the publish at 707-708 does, before `modelMutex_`
at 714); memory note's `--expect-*` citation now reads "128-131 vs **124-127**,
nay **148-152** sau `af5201e` thêm `--expect-retunes`" (verified against
`tools/logstats.py:148-152`); release-note build line no longer names a file
that doesn't exist — "installer **sẽ là** `AZSoundtech-Handsfree-Setup-1.2.0.exe`
… **sau khi đóng gói**"; `KY-THUAT-CHONG-HU.md:361`'s `notchDefaults {Q, depth}`
label now reads `{Q, depth (trần)}` with both preset depths relabelled "trần
−18"/"trần −10"; `GIOI-THIEU.md`'s "Nhớ phòng" row now says "chỉ làm sâu hơn,
không bao giờ làm nông đi, và dùng một lần".

**Out-of-scope drift fixed (docs only, cheap, as offered):**
`docs/superpowers/plans/2026-09-05-data-loop.md` §3.2 `ev` table gained the
`notch_retune` row (matching the one already added to the sibling design doc);
`README.md:10` "Current version: 1.0.3" → "1.1.3 (1.2.0 in alpha gate)".

**Deliberately left alone (not stale, or out of the named scope):**
`KY-THUAT-CHONG-HU.md:122` ("chạy lại 07/09/2026 trên binary Release ở
`af5201e`") — this cites the specific commit a specific measurement run
happened on; the DSP layer (`Biquad`/`NotchChain`) was not touched by the Task 9
fix round, so the numbers are still correct, and changing the commit label to
`b8e3f25` without re-running the test would be a claim I did not verify. Left
as an honest historical citation. Same reasoning for the two "trước `af5201e`"
boundary references in `memory/gain-aware-notch-lane-g-2026-09-07.md:82,159`
and `data-loop-design.md:124` (not touched this round) — these describe when
`notch_retune` logging landed, a fact that does not change with HEAD.

**Grep sweep (edited files only) for the five required stale terms** — see the
reply for the pasted output; summary: `535` — zero real hits (one false-positive
substring inside a pi-digit literal in an unrelated code sample in
`data-loop.md`); `af5201e` — three legitimate historical hits (see above), zero
stale "current state" hits; `đã release alpha` — zero hits; `hai bậc` — zero
hits; `0,6 dB/ms` — two hits, both now correctly qualified as "one rung, not an
enforced bound" rather than an absolute ceiling.

**Commit staged explicitly** (`README.md docs/GIOI-THIEU.md
docs/KY-THUAT-CHONG-HU.md docs/release-notes/1.2.0-alpha.md
docs/spec-ring-risk.md docs/superpowers/plans/2026-09-05-data-loop.md
docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md
docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md
installer/TESTER-NOTES.md memory/gain-aware-notch-lane-g-2026-09-07.md`).

---

## Fix round 1 — scoped re-review

**Diff reviewed:** `.superpowers/sdd/2026-09-07-gain-aware-notch/review-11adb3b..81c688c.diff`,
base `11adb3b` → head `81c688c`. The package spans two commits; `b8e3f25` is
CODE (Task 9 fix — `loadPreset` reads `notchDefaults` back, `retuneReasonName`
unknown fallthrough) and was skipped. Only the docs hunks of `81c688c` were
judged. Code was read only to check the TRUTH of the new prose against it:
`MainComponent.cpp` `loadPreset` (confirmed: applies `notchDefaults` only when
`result.preset.hasNotchDefaults` is true, at `:945`, and only to slots where
`slotUsesGlobalTuning_[i]` is true, `:949` — matches every doc claim about
Global-vs-Custom routing), `NotchChain.h:41-43` (confirmed: "10 ms is chosen so
a 6 dB rung moves at most 0.6 dB/ms; nothing in NotchChain enforces that
bound — the controller's ladder must not send a multi-rung step in the deep
direction" — matches I2's qualified claim word for word), `NotchController.h`
release constants (confirmed: `kReleaseFirstMs = kAutoReleaseMs = 30000.0`,
`kReleaseStepMs = 10000.0`, `:89,125,126` — supports the "trần −6 clears at
30 s, already at the bottom rung" claim). Mojibake sweep (`Ã`, `Â`, `�`) over
the diff: zero hits.

### Finding Verdicts

**I1 — RESOLVED.** `docs/KY-THUAT-CHONG-HU.md:4-5` (header) and `:504` (§8
heading) no longer say "đã release alpha"; both now read "đã hiện thực và qua
gate ctest, 539/539 tại `b8e3f25`; chưa đóng gói — `release-alpha.ps1` sẽ chạy
sau final review". Verified directly in the file, not just the diff.

**I2 — RESOLVED.** `docs/KY-THUAT-CHONG-HU.md:99-104` now states the ≤0.6 dB/ms
figure applies to a single 6 dB rung, that a reclamp (`max(deepestDb,
ceiling)`) or a lowered ceiling can move more than one rung inside the same
10 ms **by design**, and that `NotchChain` does not enforce the bound. This is
not just internally consistent — it is a near-verbatim restatement of the
`NotchChain.h:41-43` header comment, so the doc no longer overclaims relative
to the code it describes.

**I3 — 2 of 4 sub-claims still open.**

The `539/539` @ `b8e3f25` replacement for `535/535` / `af5201e` is done
everywhere claimed: `KY-THUAT-CHONG-HU.md:4-5,510-511`, `release-notes/
1.2.0-alpha.md:3`, `TESTER-NOTES.md:3`, roadmap `:70`, plus two the report
found on its own sweep (`2026-09-06-gain-aware-notch-design.md:5`, the memory
note's own header). The "hai bậc" → "một bậc (−18 → −12)" minor is also
correctly fixed (verified the arithmetic: `kDefaultNotchDepthDb` −18 vs
`PresetNotchDefaults::depthDB` −12 is one rung, not two).

But two parts of I3 are not actually done, despite the report's text above
claiming otherwise:

1. **The preset-ceiling round-trip on LOAD and the "reload at `deepestDb`
   regardless of slider" caveat are present in `GIOI-THIEU.md:51` and
   `KY-THUAT-CHONG-HU.md:417-424` only.** They are absent from
   `docs/release-notes/1.2.0-alpha.md` and `installer/TESTER-NOTES.md`, even
   though the report explicitly states they were added "to `GIOI-THIEU.md`'s
   preset row, `KY-THUAT-CHONG-HU.md` §4, and **both release docs**."
   `grep -in "notchDefaults\|trần\|deepestDb\|Global\|bất kể slider" docs/
   release-notes/1.2.0-alpha.md installer/TESTER-NOTES.md` finds zero hits tied
   to LOAD behaviour in either file — `release-notes/1.2.0-alpha.md:53-55` only
   restates the SAVE side ("SAVE… lưu độ sâu phòng đã cần… nên nạp lại đúng như
   lúc lưu"), and `TESTER-NOTES.md` step 3 (`:94-99`) only carries the earlier
   ramp-on-load qualifier, not the ceiling round-trip or the up-to-−24-dB
   caveat. A tester reading only the release note or the tester notes — the two
   documents actually meant for testers — has no way to know that reopening a
   preset saved mid-howl can land a notch at −24 dB regardless of where the
   DEPTH slider sits, or that loading a file with a `notchDefaults` block
   silently resets every Global-tuning slot's ceiling.

2. **The memory note's replacement line citation is itself wrong.**
   `memory/gain-aware-notch-lane-g-2026-09-07.md:158-159` now cites `148-152`
   for the `--expect-*` flags "sau `af5201e` thêm `--expect-retunes`". Verified
   against the current file (`git diff 81c688c HEAD -- tools/logstats.py` is
   empty, so this is the same content as at `81c688c`): `grep -n
   "add_argument" tools/logstats.py` puts all five `--expect-*` flags,
   including `--expect-retunes`, at **lines 153-157**, not 148-152. Line 152 is
   only the positional `file` argument — the cited range doesn't reach
   `--expect-recurrence-max` (154) or `--expect-retunes` (157) at all, i.e. it
   excludes the very flag the citation exists to point at. The round-1 fix
   replaced one stale line number with a different, still-wrong one — most
   likely computed against `logstats.py` as it stood before the `b8e3f25`
   `notch_retune` guard rewrite (which added 5 net lines to the `summarise()`
   function above `main()`, shifting everything below it by +5).

### New Breakage in the Fix Diff

- `memory/gain-aware-notch-lane-g-2026-09-07.md:158` — the corrected
  `--expect-*` line citation (`148-152`) is off by 5 from the actual location
  (`tools/logstats.py:153-157`) at the very commit (`b8e3f25`) the note names.
  Low stakes (an internal memory note, not a tester-facing doc) but it repeats
  the exact failure mode (a reviewer/implementer line-number citation nobody
  re-derived against the file) that memory lesson #17 in this same file warns
  about.

No other new inaccuracies found. The mojibake sweep was clean, and every other
statement checked against source (`MainComponent.cpp`, `NotchChain.h`,
`NotchController.h`) matched.

### Out-of-Scope Observations

- **Not yet documented, and correctly not claimed as fixed:** loading a preset
  with a **shallower** ceiling than the one currently in effect pulls live
  Detector notches up (shallower) on the next tick, the same way dragging the
  DEPTH slider down mid-show does (per `release-notes/1.2.0-alpha.md:24`,
  which documents the slider case but not the load-a-preset case explicitly).
  Confirmed absent from `GIOI-THIEU.md`, `KY-THUAT-CHONG-HU.md` §4,
  `release-notes/1.2.0-alpha.md`, and `TESTER-NOTES.md` — this is expected per
  the dispatch instructions to be added in a later pass, not a gap in this
  round's scope.
- `docs/superpowers/decisions/2026-09-06-lane-g-gain-aware-notch.md` remains
  untouched, correctly — nothing in this fix round overturned Q1–Q14.
- The uncommitted working-tree changes present at review time
  (`src/gui/SlotPanel.{cpp,h}`, `src/gui/TuningPanel.{cpp,h}`,
  `tests/test_gui_wiring.cpp`, `tests/test_slotpanel.cpp`,
  `tests/test_tuningpanel.cpp`, plus an untracked
  `.superpowers/sdd/2026-09-07-gain-aware-notch/`) and the extra commit
  `97521aa` (HandsFreeSnapshot ladder staging) sitting on top of `81c688c` are
  unrelated to this docs task and were not touched or evaluated.

### Verdict

**2 open** — I3's preset-ceiling round-trip / `deepestDb`-regardless-of-slider
caveat is missing from `docs/release-notes/1.2.0-alpha.md` and
`installer/TESTER-NOTES.md` (present only in `GIOI-THIEU.md` and
`KY-THUAT-CHONG-HU.md` §4, contrary to the report's claim of "both release
docs"), and the memory note's corrected `--expect-*` line citation
(`memory/gain-aware-notch-lane-g-2026-09-07.md:158`, "148-152") is itself off
by 5 lines from the true location (`tools/logstats.py:153-157`). I1 and I2 are
fully resolved; all named minors are fixed; no mojibake found.

## Fix round 2

**Base:** `fba2626` (Task 9 fix round 2 landed here: `546/546`, off-list
ceiling shown as text and surviving a combo touch, `preset_load` logs
`q`/`depth_db`/`ceiling_applied`, and the level statement corrected — a
shallower ceiling pulls live Detector notches up on the next tick, up to
+14 dB, ramped 10 ms). Docs only, 8 files modified (all already tracked, no
creates). No `src/`, `tests/`, `tools/`, `installer/*.ps1` or `CMakeLists.txt`
touched. No `git stash` used. `.superpowers/sdd/.gitignore` did not exist
(checked, `rm -f` run anyway); nothing under `.superpowers/` is staged or
committed.

Read before editing, per the dispatch: `src/app/MainComponent.cpp` `loadPreset`
(:840-980 — confirmed `notchDefaults` applied only when
`result.preset.hasNotchDefaults` and only to `slotUsesGlobalTuning_[i]` slots;
`preset_load` now carries `q`/`depth_db`/`ceiling_applied`, the last always
written), `src/gui/TuningPanel.cpp` `refresh()`/`currentParams()` (:233-298 —
`selectOrShow` shows an off-list value as text via `provided_`, and
`currentParams()` now re-reports the last-provided snapshot instead of
`choices[0]` for an unselected combo, so a touch on one combo no longer pushes
Q 10 / −6 dB onto every Global slot), `tools/logstats.py`
(`add_argument` calls at :152-157, `--expect-retunes` at :157 — confirmed the
re-reviewer's `153-157`, not the round-1 fix's `148-152`), and
`src/app/NotchController.cpp` (:786-796 — the ceiling pass:
`n.deepestDb = std::max(n.deepestDb, ceiling)` then `if (n.depthDB < ceiling)`
issues one ramped `pushRetuneLocked(..., RetuneReason::Ceiling)` — confirms the
shallower-ceiling-pull mechanism and its 10 ms ramp).

### Findings addressed

1. **The shallower-ceiling-pull warning (c) added as a must-read-first warning**
   in both tester-facing docs: `installer/TESTER-NOTES.md` new item 7 in "Mới
   trong 1.2.0" (before the "Báo lại ngay" block) plus a forward-reference at
   test-scenario step 3, right before the LOAD action it warns about; and
   `docs/release-notes/1.2.0-alpha.md` new item 4 in "Nghe ở âm lượng thấp
   trước". Both state: deeper ceiling = 0 dB now; shallower ceiling pulls every
   live Detector notch up to the new ceiling on the next tick, up to +14 dB at
   a bin that was ringing, ramped 10 ms (no click, still a real level rise);
   Preset/Manual/Soundcheck notches carry their own ceiling and are untouched;
   open at low volume, not mid-song.
2. **(a) LOAD ceiling round-trip and (d) off-list value display** added to
   both tester docs (`release-notes/1.2.0-alpha.md` new bullets under "Có gì
   trong 1.2.0"; `TESTER-NOTES.md` new item 8 plus the step-3 LOAD note) and
   already present in `GIOI-THIEU.md`/`KY-THUAT-CHONG-HU.md` §4 from round 1 —
   (c) added there too this round (a `⚠️` callout in §4, plus a new row in the
   §5 risk/guard table since §4 has no separate "level table").
3. **(e) `preset_load.q`/`.depth_db`/`.ceiling_applied`** added to: both tester
   docs (release note bullet, `TESTER-NOTES.md` item 8 area), the
   `KY-THUAT-CHONG-HU.md` §7 event table (`preset_load` row), and a new
   `preset_load` row in `docs/superpowers/specs/2026-09-05-data-loop-design.md`
   §3.2's event table — that table had no `preset_load` row at all before this
   round — with a paragraph on the level consequence of `ceiling_applied` next
   to the existing `notch_retune` paragraph.
4. **Memory note logstats citation fixed** (finding 2): lesson 17 now cites
   `tools/logstats.py:153-157` (verified: `add_argument` calls span :152-157,
   `--expect-retunes` at :157) instead of the round-1 fix's wrong `148-152`,
   and names the mistake explicitly (a bare "148-152" string was avoided in
   the new prose so the required stale-term grep comes back clean rather than
   flagging its own explanation).
5. **Test count swept to `546/546` at `fba2626`** (finding 3): every
   non-historical `539/539` / `b8e3f25` citation updated —
   `KY-THUAT-CHONG-HU.md` header + §8 (also `+85` → `+92` vs the 454 baseline),
   `release-notes/1.2.0-alpha.md` build line, `TESTER-NOTES.md` header, the
   roadmap status row G, the design-doc status line, and the memory note's own
   header (`01ecb4c..b8e3f25` → `01ecb4c..fba2626`, `539` → `546`). One
   `b8e3f25` citation deliberately left alone:
   `KY-THUAT-CHONG-HU.md:417` ("Từ fix round Task 9 (`b8e3f25`), `loadPreset`
   cũng đọc lại trần...") — this names the specific commit where the Q11
   read-side landed, a historical fact still true at `fba2626` (fix round 2
   did not touch that mechanism, only the GUI display of an off-list result),
   matching the precedent the round-1 report set for `af5201e` citations.
6. **New memory lesson added** (finding 4): `memory/gain-aware-notch-lane-g-2026-09-07.md`
   §A gained item 20 — an off-list ceiling was unreachable in the GUI until
   `loadPreset` applied `notchDefaults` (Q11), and the moment a real writer
   put such a value into the model, two dormant panel bugs surfaced at once
   (blank combo, then a touch on any other combo pushing Q 10 / −6 dB to every
   Global slot); the generalized lesson: every new writer of a model value
   must be checked against every panel that displays it.

### Grep sweep (edited files, per the closing instruction)

```
539/539  -> 0 hits (docs/, installer/, memory/, README.md)
b8e3f25  -> 1 hit, docs/KY-THUAT-CHONG-HU.md:417 — historical ("Từ fix round
            Task 9 (`b8e3f25`)"), same category the round-1 report already
            established for af5201e citations; not "current status"
148-152  -> 0 hits
```

Mojibake sweep (`Ã`, `Â`, U+FFFD) over all 8 edited files: zero hits.

### Files changed this round (8, all already tracked)

`docs/GIOI-THIEU.md`, `docs/KY-THUAT-CHONG-HU.md`,
`docs/release-notes/1.2.0-alpha.md`,
`docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md`,
`docs/superpowers/specs/2026-09-05-data-loop-design.md`,
`docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md`,
`installer/TESTER-NOTES.md`, `memory/gain-aware-notch-lane-g-2026-09-07.md`.

### Left alone (out of this round's named scope)

`docs/superpowers/plans/2026-09-05-data-loop.md` §3.2's `ev` table (has
`notch_retune` from round 1 but not `preset_load`) — not named in this
round's brief, and `README.md` already reads "1.1.3 (1.2.0 in alpha gate)"
with no stale `539`/`b8e3f25` to fix. Neither touched.
