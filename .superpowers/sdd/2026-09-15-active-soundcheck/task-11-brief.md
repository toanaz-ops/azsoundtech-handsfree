### Task 11: docs, tester notes, release notes, memory, roadmap

**Mức level dự kiến:** **0 dB** — no code. This task exists because a behaviour change nobody wrote down is a behaviour change the next session will "fix" back. Every level figure below is **copied from spec §3 and from the numbers the earlier tasks actually printed**, never invented here.

**Three claims this task must carry because no test can** (spec §5.3):

1. **What −20 dBFS sounds like in a real room** is unknown to this project. The app has no idea of absolute SPL and cannot convert without knowing the desk's and the amp's gain. The only honest sentence is the one spec §3 gives: *"the sweep plays 20 dB below the system's full scale, at your current master setting."* Rev 1 of the spec said "≈ 80 dB SPL"; that figure rested on an assumption about the desk that the app is not entitled to make, and it was removed.
2. **Whether preventive notches actually raise gain-before-feedback** is the one number that proves this lane was worth building, and only a rig can produce it: turn the master up a dB at a time until it howls, before and after `ÁP DỤNG`, and report the difference.
3. **Whether 4.5 s of silence per channel — ~72 s for a big system — fits a real soundcheck workflow.** That is a process change, not just a feature.

**Files:**
- Modify: `docs/GIOI-THIEU.md`
- Modify: `docs/KY-THUAT-CHONG-HU.md`
- Modify: `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md` — line **24** (the lane table's M row) and line **71** (the status table's M row)
- Modify: `docs/superpowers/specs/2026-09-15-active-soundcheck-design.md` — the status line
- Modify: `installer/TESTER-NOTES.md`
- Create: `docs/release-notes/1.3.0-alpha.md`
- Create: `memory/active-soundcheck-lane-m-2026-09-15.md`; modify `memory/MEMORY.md`

**Interfaces:** none — prose only.

- [ ] **Step 1: `docs/GIOI-THIEU.md`**

Add a row to the feature table describing the active soundcheck for a soundman, in the voice the file already uses. It must say: a separate **`ĐO`** button; the app plays a 3 s sweep out of **one output channel at a time**; **that channel goes completely silent for 4.5 s** while it is measured, so a 4-output system takes about 18 s and an 8-slot stereo system about **72 s**; the app **proposes** notches and places nothing until **`ÁP DỤNG`**; above 6 kHz this version **draws but never proposes**; and **turn the master down first**.

Amend the preset row to state the asymmetry (spec §4.8): a preset saved after a soundcheck **does** carry the preventive notches, but reloading brings them back as `Origin::Preset`, so they **auto-release after 30 s of quiet** like any other preset notch. To get the full preventive protection back, run `ĐO` again.

- [ ] **Step 2: `docs/KY-THUAT-CHONG-HU.md`**

A new section, `### Soundcheck đo chủ động (1.3.0, lane M)`, covering:

- the capture hoist and the three insertion points in the callback, **why the capture pointer must NOT be taken inside the lane loop** (a measurement mic is not in the routing table), and **why point 3 must sit above the `±1.0f` clamp** (`src/app/AudioEngine.cpp:631-647`);
- the mute rule: **every lane routed to the measured OUTPUT CHANNEL**, not one `(slot, lane)` pair — because several slots sum onto one channel (`:565-577` clears, `:621` accumulates), so muting a pair leaves that channel's loop closed;
- the seven atomics and why they are read **once** beside `bypass`;
- why tap suspension is keyed on `scSuspendTaps_` and not on `scOutChannel_`, **with the 11.5 s number**;
- the loop-gain maths, `H_dB >= 0` ⇒ howl, `margin = -H_dB`, and the truncation caveat the test measures (the algebra is in the test's own comment, not just the number);
- that lane M neither writes nor consumes lane G's room memory, and **which two lines hold that**: `src/app/NotchController.cpp:1116` stops a Soundcheck placement from CONSUMING an entry, `:1169` stops a remembered depth from DECIDING a Soundcheck depth. Both live in `placeConfirmed`, which is why the test that covers them drives a real detector placement;
- the trusted band, and why 6–10 kHz is drawn but never proposed (the 20 dB per-bin energy difference);
- the depth rule and the six worked examples, with the note that the **ceiling is the last rung** and may be off-grid (`presets/Music.json` = −10);
- the abort set and the worst-case stop latency table from spec §3;
- the preset asymmetry from §4.8 — **this file and `GIOI-THIEU.md` must both carry it, in the same change** (Definition of done item 5).

Add to the safety-limits table:

```markdown
| Sweep vượt mức | `SoundcheckSignal::clampPeak` kẹp ở `kSoundcheckMaxPeak` = 0.1f (−20 dBFS) tại CẢ chỗ đặt lẫn chỗ dùng; kẹp cuối đường ±1.0f vẫn nằm SAU điểm tiêm |
| Thread lane M chết giữa lúc phát | Ramp-out do CHÍNH callback sinh; nó tự đặt `scOutChannel_ = -1`. Không cần thread nào khác còn sống |
| Restart thiết bị giữa lúc đo | `onBeforeRestart` abort + join lane M TRƯỚC khi stop detector; `micCapture_.clear()` trong cùng khối drain |
| Chỉ số kênh ngoài phạm vi sau restart | Kiểm lại MỖI callback với số kênh của chính callback đó |
```

And to the session-log table:

```markdown
| `soundcheck_start` / `_output` / `_result` / `_apply` / `_abort` | lane M | xem `docs/superpowers/specs/2026-09-15-active-soundcheck-design.md` §4.7. `ring_risk` là **null** khi chưa chấm được khung nào, không phải 0.0 |
```

- [ ] **Step 3: the roadmap**

`docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md` **line 24** — the lane table's M row still points at a spec filename that does not exist (`2026-09-06-active-soundcheck-design.md`) and says "phiên B theo prompt 2026-09-06 chưa chạy". Replace the last column with a link to the real file:

```markdown
[`2026-09-15-active-soundcheck-design.md`](2026-09-15-active-soundcheck-design.md) — **đã hạ cánh 1.3.0**
```

**Line 71** — the status table's M row currently reads `**mở** — S đã hạ cánh; chưa có spec, chưa có nhánh`. Replace it with the branch, the commit range, the suite count the final `ctest` printed, and the nine owner confirmations with the date each was answered.

And lane A's row — **line 74**, `| A | chờ M (G đã merge: fallback notch dùng thang G) | 2026-09-07 |`: M now exists, so amend "chờ M" to name what lane M does and does **not** supply — it gives loop gain per bin, and it explicitly does **not** give round-trip delay (spec §4.4, §6).

- [ ] **Step 4: the spec's own status line**

`docs/superpowers/specs/2026-09-15-active-soundcheck-design.md` line 4 currently says **"CHƯA owner duyệt"**. Update it to name the plan, the branch and the owner's decision date — and **only after Task 12 Step 0 has recorded real answers**. Until then it stays as it is: a spec that claims owner approval it does not have is worse than one that admits it.

- [ ] **Step 5: `docs/release-notes/1.3.0-alpha.md`**

Create it in the shape of `docs/release-notes/1.2.0-alpha.md`. In this order: version and date; the suite count from the final `ctest` run; what changed; and an explicit **"nghe ở âm lượng thấp trước"** block quoting spec §3's table row by row:

- on the measured output channel: 4.5 s silent, then a −20 dBFS peak / ≈ −23 dBFS RMS sweep for 3.0 s;
- on every other output channel: 0 dB;
- worst case ~72 s for 8 stereo slots;
- after `ÁP DỤNG`: −6 / −12 / −18 / −24 dB (or the preset ceiling) on up to 6 bins per lane;
- after `BỎ` or the 20 s timeout: nothing at all;
- **there is no limiter in this app** — the only thing between the sweep and the driver is a ±1.0f clamp, and the real loudness is set by the operator's master.

It must also state the two divergences from the owner's original prompt that Q6 and Q16 record: v1 **proposes** rather than places, and `ĐO` is its **own** button.

- [ ] **Step 6: `installer/TESTER-NOTES.md`**

Update the header block (version, build date, suite count, SHA-256 and size — the SHA comes from the installer the release script actually produces, so fill it in **after** Task 12). Add `## Mới trong 1.3.0` **before** `## Mới trong 1.2.0`, written for a soundman:

- **HẠ MASTER TRƯỚC.** The sweep plays at 20 dB below the system's full scale **at your current master**. If the system is running a show at normal level, the sweep is a little quieter than the programme. If the master is wide open, 20 dB below full scale is still very loud. The level can only be turned **down**, never up.
- Press `ĐO`; confirm the dialog; **the channel being measured goes completely silent for 4.5 seconds**, one channel after another. A 4-output system takes about 18 s; an 8-slot stereo system about **72 s**. Do not press it while the MC is talking.
- **`DỪNG` is the stop button.** `Esc` usually works too but is not guaranteed — it only reaches the overlay while the overlay has focus. From pressing `DỪNG` to silence is about 31 ms at a 64-sample buffer and 51 ms at 1024, plus your driver's and amp's own latency.
- **The app only proposes.** Nothing changes until you press `ÁP DỤNG`. `BỎ`, or waiting 20 seconds, throws the proposal away.
- **Above 6 kHz this version draws but does not propose.** The sweep covers 100 Hz – 10 kHz, but a constant-amplitude log sweep puts 20 dB less energy into each bin at 10 kHz than at 100 Hz, so the top of the range is not trustworthy yet. Tell us whether it is worth widening.
- **The loop through OTHER output channels is still closed while measuring.** Their loudspeakers are still carrying the mic. If the room is near the edge on one of them, the sweep can set it off — which is why the app refuses to start when `RING RISK` already reads `RISING` or worse.
- **A preset saved after a soundcheck brings the notches back as PRESET notches**, so they auto-release after 30 s of quiet. Run `ĐO` again to get the preventive protection back in full.
- **What to report**, in this order of value:
  1. Turn the master up a dB at a time until it howls, **before and after** `ÁP DỤNG`. **The difference is the only number that proves this feature works.**
  2. Compare the margin curve against a reference measurement (REW / Smaart and a measurement mic): **how many dB out, how many bins off?**
  3. Press `DỪNG` mid-sweep: how long until silence, and is there a click?
  4. Does 4.5 s of silence per channel fit your soundcheck routine? Is ~72 s too long?
  5. In a noisy room, how many channels come back "không đo được"?
  6. Do lane M's candidates match the bins the detector actually notches in the show afterwards?

- [ ] **Step 7: `memory/`**

Create `memory/active-soundcheck-lane-m-2026-09-15.md` in the shape of the other notes, recording at minimum:

- **The unit trap, twice in one project.** `peakinessAt` is an **unbounded ratio** (noise 7.35, tone 131.70) and `kConfirmScore` is the threshold of a **0..1 product**. Spec rev 2 compared them and would have aborted every run in every room; lane R had made the mirror-image mistake three weeks earlier. **Name the unit of both sides of every comparison, in a comment, at the point of comparison.**
- **A flag that returns to a sentinel mid-run cannot be a run-scoped lock.** `scOutChannel_` goes to −1 at every Gap, so keying tap suspension on it un-suspends for 300 ms per channel — ≈ 11.5 s of release clock over 16 channels, past `kReleaseStepMs`. The fix is a second flag with the right scope, not a cleverer read of the first.
- **A safety property must not be inferred from the sign of an index.** `scCaptureActive_` exists separately from `scOutChannel_` for exactly that reason.
- **Where a publish sits decides what a reader can see.** `copySnapshot()` is refreshed inside the drain loop (`NotchController.cpp:568-582`, `:617-650`), so suspending the taps freezes it — which is why the overlay draws lane M's own data and the `index` read happens only after the taps resume.
- **The sign of a derived quantity is worth a test of its own.** Rev 1's `needed = -(H_dB + 6)` produced a POSITIVE depth, which `setNotchImpl` refuses at `NotchController.cpp:214` — so the whole feature would have placed nothing, silently.
- **State a quantiser as a rule, then check every example against the rule.** Rev 2's wording ("the deepest rung not deeper than depth_raw") contradicted its own worked examples and its own tests. Six examples re-derived by hand caught it.
- **`SnapshotBuffer::linked` is the operator's switch, not the behaviour.** `effectiveLinked()` is `isLinked() || width_ < 2 || taps_[1] == nullptr`; from outside, the equivalent is `snapshot.linked || snapshot.laneCount < 2`.
- **Three review rounds found 8 / 2 / 0 blockers, and every blocker in round 2 was created by the round-1 rewrite.** Absorbing eight blockers in one pass generates new defects at a meaningful rate. A second review of the fixed document is not ceremony.
- **The invariant no test can hold.** Invariant 16 ("no lock, no allocation, no logging in the callback") is a property of the source text. It is in the reviewer checklist, and the plan says outright that nothing in the suite enforces it — better than pretending a test does.

And the lessons the plan's own read-only cross-check produced, which are about writing plans rather than about DSP and cost the most:

- **A plan is not verified until someone opens the files it cites, and "flagged as unverified" is not the same as verified.** Rev 1's self-review named four unverified helpers. The cross-check opened the files and found nine more problems behind them, including two that would have shipped as production defects. Lane G learned this once (M-1); one lane later it was still true.
- **"Re-read a shared snapshot before every write" is not an allocator.** `copySnapshot()` republishes on the detector thread at hop cadence (~10.7 ms) while an APPLY loop runs in microseconds on the message thread, so six re-reads return one frame and six placements land on one index. The thing that actually allocates has to be **local to the call**. Re-reading is still worth doing — as a guard against the OTHER writer — but naming it the allocator hid the bug behind a plausible sentence.
- **A capture path gated on the routing table captures nothing.** The measurement mic is, by definition, not a lane. Ask of every "read this channel" line: *whose* table decides whether that channel is visible?
- **A test seam has to be on the far side of the thing it defeats.** A seam on the peak proves nothing when the signal clamps the peak AND the sample; the seam has to multiply what the signal already produced. A seam that cannot turn its invariant red is decoration.
- **A fixture number needs its algebra written beside it, and the algebra has to be run.** Rev 1 shipped a 3.0 dB truncation expectation next to a derivation that yields 0.0035 dB, a factor of 750. Nobody spotted it because the derivation was prose and the number was code.
- **Check the sign twice when a quantity is defined as the negation of another.** `marginDb == -hDb`, so "sorted hottest first" means margin ASCENDING. The test asserted the opposite and would have failed on correct code.
- **Enum-to-string tests must loop the enumerators.** A `std::set` of eight literals asserts that eight literals differ.

Add one line to `memory/MEMORY.md`'s `## Notes` list, in the same style as its neighbours, linking the new file.

- [ ] **Step 8: Verify and commit**

```bash
cd build && ctest -C Release
```
Expected: `100% tests passed` — unchanged from Task 10; record the number for the release note and the tester notes.

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add docs/GIOI-THIEU.md docs/KY-THUAT-CHONG-HU.md docs/release-notes/1.3.0-alpha.md installer/TESTER-NOTES.md memory/MEMORY.md memory/active-soundcheck-lane-m-2026-09-15.md
```
```bash
git add docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md docs/superpowers/specs/2026-09-15-active-soundcheck-design.md
```
```bash
git commit -m "docs(lane-m): active soundcheck -- level table, trusted band, depth rule, preset asymmetry, tester notes"
```

---

