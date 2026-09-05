# Task 8 report — screenshot with verdict states, docs, release note, roadmap status

Branch `feat/data-loop`, worktree `peakiness-sweep-tool-c81129`. Commits: `e3a5747` (snapshot),
`fd95769` (docs+memory), on top of `1ca01e9`.

## Step 1: snapshot.cpp

`tools/snapshot.cpp` — after the placement loop's final `refreshFromSnapshot()` pair and
before the `console-live.png` shoot, inserted a block that finds rows by `rowForTest(i).freq`
text ("247 Hz", "1.9 kHz") rather than index, fails loudly via `std::cerr` + `return 1` if
either is missing, then calls `goodButtonForTest(goodRow)->onClick()` and
`falseButtonForTest(falseRow)->onClick()` with no pump afterwards. Built
`HandsFreeSnapshot` target, then the full tree (`cmake --build build --config Release`,
both succeeded, MSBuild/VS 18 2026). Rendered `shots/console-idle.png` and
`shots/console-live.png` (1440x920, `--fast`).

**What I saw in console-live.png**: VERDICT header visible over the ACTIVE NOTCHES table.
Row 01 (247 Hz, L, −18.0 dB, Q 30.0) reads plain text **GOOD** in the VERDICT column. Row 02
(1.9 kHz, L, −21.0 dB, Q 30.0) reads plain text **FALSE** in red. Row 03 (1.2 kHz, R,
−12.0 dB, Q 30.0) still shows both **GOOD** and **FALSE** buttons (unjudged). HELD column
(age) is present on all three rows and not truncated. Analyser shows three notch markers
(01, 03R dashed, 02) at their frequencies. Nothing clipped or overlapping.

**console-idle.png**: unchanged empty state ("WAITING FOR SIGNAL" / "NOTHING RINGING"),
no verdict rows since no notches exist yet.

Both PNGs sent to the user via SendUserFile with a caption naming the three states.

## Step 2: docs/KY-THUAT-CHONG-HU.md

Inserted the brief's exact "## 7. Log session và nhãn (lane D, v1.1.2)" section (event
table, no-audio commitment, outbox/mutex note, FALSE-clears/GOOD-labels-only, `ref` frame
semantics, `logstats.py`, lane C gate) immediately before the old "## 7. Trạng thái"
section, which is renumbered to "## 8." with:
- opening sentence: "Bản 1.1.2 thêm vòng dữ liệu (lane D): nút GOOD/FALSE, log session
  JSONL, `tools/logstats.py`."
- suite count updated 402 → **430/430**, with a short breakdown of what the +28 new tests
  cover (SessionLogger, event/outbox delivery outside modelMutex_, verdict buttons,
  logstats fixture).
- "Còn mở" list kept the existing items (code signing, HW integration, RING RISK,
  laneAsymmetryBonus) and added lane C's ≥300/≥3-session gate plus the open owner
  decision on re-entering-lane `riseReferenceMs` gating after a widen (A-9 rundown,
  pointing at the SDD ledger's "Task 3: CONTROLLER RULING").
- §5 safety table got one new row: `| Logger chặn detector thread | Hàng đợi có cap 4096,
  bỏ + đếm; sink gọi ngoài modelMutex_ |`.

Left the doc's top-matter line (still says "v1.1.0") untouched — that drift predates this
task and wasn't in the brief's scope; flagging it here rather than silently fixing.

## Step 3: docs/GIOI-THIEU.md

Added the feature-table row "**Chấm notch để app học**" right after "Stereo độc lập", and
a new "## Chấm notch để app học" section right after "## Theo dõi khi chạy" — both exactly
the brief's text.

## Step 4: docs/release-notes/1.1.2-alpha.md (new)

Wrote using the brief's exact body (0 dB stated up top, GOOD/FALSE behaviour, log path and
retention, no-audio commitment, `logstats.py`, tester instructions, what-to-do-if-FALSE-
doesn't-clear-in-1s) plus one short added closing paragraph explaining why the data matters
and restating the ≥300 verdicts / ≥3 sessions gate for lane C, matching the tone of
`1.1.1-alpha.md`.

## Step 5: roadmap + spec status

`docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md` — status table row D set to
"đã làm xong" on `feat/data-loop` (8 task, suite 430/430, review từng task + verifier độc
lập toàn nhánh đang chờ; release 1.1.2 alpha; chờ owner PR). Row C set to "chờ D nhãn: mở
khi có ≥ 300 verdict từ ≥ 3 session". Added a note below the table recording the
controller ruling on amendment A-9 (widen-reset removed from lane D; half-a-reset left
CandidateScorer history stale and made the returning lane MORE permissive than shipped
1.1.1; whether to gate `riseReferenceMs` on a re-entering lane is an open owner decision
for lane S), pointing at `.superpowers/sdd/2026-09-05-data-loop/progress.md`, "Task 3:
CONTROLLER RULING".

Note: the brief text named branch `claude_desk/lane-d-data-loop-239597`; I used the
actual branch this work is on, `feat/data-loop` (confirmed via `git branch --show-current`
and the task context), since the brief could not have known the real branch name.

`docs/superpowers/specs/2026-09-05-data-loop-design.md` — replaced the "Trạng thái" line
at the top with one pointing at `docs/superpowers/plans/2026-09-05-data-loop.md`'s
"Hợp nhất hai phiên" section and its "Amendments to the spec" table (A-1..A-10) — found by
grepping the actual checked-in plan (not the SDD-worktree draft plan, which uses different
P-# labels and is a separate, earlier-iteration document). Did not touch the rest of the
spec body.

## Step 6: full suite

```
cd build && ctest -C Release
...
100% tests passed, 0 tests failed out of 430
Total Test time (real) =  29.15 sec
```
Run right after the full `cmake --build build --config Release` (which also rebuilt
`HandsFreeSnapshot` and the main app with the snapshot.cpp change already in place).

## Memory

Created `memory/data-loop-lessons-2026-09-05.md` (title + "Bối cảnh" paragraph, no YAML
frontmatter — matched the actual convention in `memory/stereo-lane-lessons-2026-09-05.md`,
which also has no frontmatter block despite the brief's wording). Six lessons: JSON double
serialisation forcing pre-rounding to 3 sig figs; `var::clone()` needed before stamping `t`
because `getDynamicObject()` mutates through a const ref; never start the logger in
`MainComponent`'s ctor; headless tests must call `onClick()` not `triggerClick()`; the
widen-reset half-measure (A-9) being more permissive than no reset; and two multi-session
process traps (duplicate session on one lane via a re-pasted prompt, and a worktree pruned
by name-pattern matching without checking `git worktree list` + running sessions' cwd).
Indexed with one line in `memory/MEMORY.md` under "## Notes", same style as existing rows.

## Commits

- `e3a5747` chore(snapshot): console-live shows unjudged, GOOD and FALSE verdict rows (lane D)
  — `tools/snapshot.cpp` only.
- `fd95769` docs: lane D — session log, verdict buttons, tester note 1.1.2, roadmap status,
  memory lessons — the seven doc/memory files listed in the task, explicit paths, no `-A`.

Neither commit touched `.superpowers/sdd/2026-09-05-data-loop/progress.md` (already modified
before this task started, out of scope) nor any untracked SDD brief/report files — those are
left for the orchestrating session's ledger commit per the plan's closing steps.

## Concerns

- The brief's given branch name for the roadmap row (`claude_desk/lane-d-data-loop-239597`)
  does not match the real branch (`feat/data-loop`); used the real one and noted it above.
- `docs/KY-THUAT-CHONG-HU.md`'s top-matter still says v1.1.0 (pre-existing drift, out of
  this task's file list) — worth a follow-up pass but not touched here.
- The "amendment table" the brief points at lives in `docs/superpowers/plans/2026-09-05-data-loop.md`,
  not in `.superpowers/sdd/2026-09-05-data-loop/plan-session-B-draft.md` (which uses a
  different P-1..P-9 numbering and reads as an earlier/parallel draft) — worth reconciling
  which plan is canonical before the next lane starts from this one as a template.
- No build was needed for the docs-only commit; only the snapshot commit required rebuilding,
  and the full suite was re-run afterward to gate both commits together (430/430 covers the
  state after both).

## Fix round 1 report

Reviewer findings (2 Important, 1 Minor) fixed, docs-only, no build needed.

1. **Important — missing frontmatter in `memory/data-loop-lessons-2026-09-05.md`.**
   Added a `---` block at line 1 (`name: data-loop-lessons`, one-line `description:`
   summarizing the note's content, `metadata: / type: project`), matching the shape
   of `memory/sdd-workspace-gitignore-trap-2026-09-04.md`. Body unchanged below the
   new block.

2. **Important — false "no re-detect on widen" claim in the tester note.**
   `docs/release-notes/1.1.2-alpha.md:13-15` (pre-fix) listed a third "sửa nhỏ": "slot
   mono → stereo không dò lại đuôi audio cũ của làn R." That behaviour (the widen 1→2
   Detector reset from amendment A-9) was withdrawn from lane D per the controller
   ruling recorded in `.superpowers/sdd/2026-09-05-data-loop/progress.md` ("Task 3:
   CONTROLLER RULING") and in `memory/data-loop-lessons-2026-09-05.md` §5 — it was never
   shipped, so the bullet was false for this build. Deleted the bullet; kept the other
   two ("LOAD… cập nhật bảng ROUTING ngay" and "preset có notch làn R cho slot mono
   được đếm và ghi log"). The intro to that list was a plain semicolon-separated
   sentence with no explicit count word ("ba"/"three") to fix — checked and confirmed
   absent, so no further wording change was needed there.
   Also grepped `docs/KY-THUAT-CHONG-HU.md` and `docs/GIOI-THIEU.md` for "đuôi audio" /
   "không dò lại" — no matches in either, so neither needed a corresponding edit.
   (One remaining hit is in `docs/superpowers/plans/2026-09-05-data-loop.md:2504`, a
   historical plan document out of this task's scope — left as-is.)

3. **Minor — stale version in `docs/KY-THUAT-CHONG-HU.md` top-matter.**
   Line 4 said "v1.1.0 (chờ release)"; changed to "v1.1.2 (chờ release)" to agree with
   the file's own §8 ("Trạng thái & kiểm chứng (05/09/2026, v1.1.2 — chờ release)").

### Checks run

```
$ for f in memory/data-loop-lessons-2026-09-05.md docs/release-notes/1.1.2-alpha.md docs/KY-THUAT-CHONG-HU.md; do printf "%s: " "$f"; head -c 3 "$f" | xxd -p; done
memory/data-loop-lessons-2026-09-05.md: 2d2d2d      # "---"  (no BOM)
docs/release-notes/1.1.2-alpha.md: 232048           # "# H"  (no BOM)
docs/KY-THUAT-CHONG-HU.md: 23204b                   # "# K"  (no BOM)

$ grep -rn "đuôi audio\|không dò lại" docs/ memory/
docs/superpowers/plans/2026-09-05-data-loop.md:2504:  ... (historical plan, out of scope)
# no hits in the release note, KY-THUAT-CHONG-HU.md, GIOI-THIEU.md, or memory/

$ head -6 memory/data-loop-lessons-2026-09-05.md
---
name: data-loop-lessons
description: lane D (data loop — SessionLogger, GOOD/FALSE verdicts, tools/logstats.py) pitfalls — ...
metadata:
  type: project
---
```

No build required (docs-only change, no header/CMakeLists/DSP path touched).
