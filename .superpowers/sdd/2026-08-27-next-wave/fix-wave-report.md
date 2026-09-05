# Lane R — final-review FIX WAVE report

**Branch** `claude_desk/merge-branches-subagent-c76c7f`, from `ac3195f`.
**Date** 2026-09-06. **Expected level change: 0 dB** — nothing in this wave
touches `src/app/NotchController*` or `src/dsp/*`.

**Headline:** I-1, I-2, I-4 and both minors are done. **I-3 shipped its docs
half only — the ruled provider guard is NOT in the tree, because it is
measurably incompatible with the headless fixture** (evidence below). The docs
now state that gap as an open gap rather than as a fixed one.

---

## I-1 — stale `ringRiskProvider` comment + task-brief citations · DONE

`src/gui/SpectrumView.h:214-227` — the block claiming "`ringRiskProvider` is
still null -- nothing assigns it yet ... Wiring it to the monitored slot is
task R3" is replaced with what is actually true: MainComponent assigns it in
its constructor to the monitored slot, the lambda returns the band RAW, and the
750 ms hold is applied in `timerCallback` (so a provider must never hold as
well). The retained sentence is that a NULL provider still renders
`Unavailable` — that is the resting state the spec pins, and it is still true.

Contract citation now points at `docs/spec-ring-risk.md` sections 2-4 and the
"Amendment lane R — 2026-09-06" block in
`docs/superpowers/plans/2026-08-27-next-wave.md`.

Minor "three different files cited as the amendment" — fixed. `task-R1/2/3-brief`
citations replaced with the plan-amendment citation in all three places:

- `src/gui/SpectrumView.h:221` (was `task-R2-brief.md`)
- `docs/spec-ring-risk.md:10` (was `task-R3-brief.md`)
- `memory/ring-risk-lane-r-2026-09-06.md:6` (was `task-R3-brief.md`)

Verified: `grep -rn "task-R[123]-brief" src docs memory installer` → no matches.

## I-2 — `installer/TESTER-NOTES.md` · DONE

Item 3 (was: "Ô RING RISK luôn hiện N/A — nguồn dữ liệu chưa nối ... Sẽ hoạt
động ở bản sau") is now a v1.1.3 reading guide matching
`docs/release-notes/1.1.3-alpha.md`: the four bands, what N/A actually means
(detection off / no history yet / just switched slot or device), CRITICAL with
all three exceptions (table full, bin guarded, persistence streak not yet met),
the 750 ms hold, and the two things to report back (red chip with no ring; ring
with the chip still LOW). File structure and numbering unchanged — it is still
items 1-3 under "Biết trước để không tưởng app hỏng".

**Deviation from the brief, deliberate:** the brief asked for "rig đang dừng" in
the N/A list. That is not true in the shipped build (see I-3), so the note says
the opposite truth instead — when the rig stops or restarts the chip *freezes on
its last reading*, don't trust it while the rig is stopped. Telling testers a
stopped rig shows N/A would have been a new false statement of exactly the kind
this wave exists to remove.

Not changed, out of scope: the file header still reads v1.0.5 / suite 368 / a
1.0.5 SHA-256. Those lines are owned by the release step (`release-alpha.ps1`
run + the Z: drop copy), not by this wave. Flagging it, not touching it.

## I-3 — stopped-rig freeze · GUARD NOT SHIPPED, docs tell the truth instead

### What was ruled

Return `gui::SpectrumView::RingRisk::Unavailable` from the MainComponent
provider lambda when `! engine_.isRunning()`, before copying the snapshot; add a
test that drives the displayed slot to Critical, stops the engine, ticks, and
asserts `Unavailable`; mutation-check it.

### What I measured

I implemented the guard exactly as ruled, built, and ran the focused suite. It
does not work, and the reason is structural, not a detail of my patch.

**No headless test ever opens an audio device.** `AudioEngine::isRunning()` is
set true only inside `AudioEngine::start()` after a real device opens
(`src/app/AudioEngine.cpp:86`), and `MainComponent::startAudio()` is the only
caller. `tests/test_gui_wiring.cpp:144` and `:389` *assert* the engine is NOT
running. Every RingRisk test drives the detector by hand instead —
`ringRiskPump()` writes into the tap buffer and calls `controller->runOnce()`
directly (`tests/test_gui_wiring.cpp:816-820`) with the engine stopped.

So `! engine_.isRunning()` is **true in every headless test**. The guard does
not merely resist testing — it makes RING RISK permanently `N/A` across the
whole suite, and it turns two currently-green wiring tests red:

```
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter=*RingRisk*
[==========] 23 tests from 3 test suites ran. (1643 ms total)
[  PASSED  ] 21 tests.
[  FAILED  ] 2 tests, listed below:
[  FAILED  ] MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms
[  FAILED  ] MainComponent.RingRiskFollowsTheMonitoredSlotAcrossASwitch

 2 FAILED TESTS
```

```
$ ./build/tests/Release/HandsFreeTests.exe \
    --gtest_filter=MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms
tests\test_gui_wiring.cpp(927): error: Expected equality of these values:
  app.getSpectrumViewForTest().getRingRisk()
    Which is: 4-byte object <00-00 00-00>      <-- Unavailable
  gui::SpectrumView::RingRisk::Critical
    Which is: 4-byte object <03-00 00-00>
[  FAILED  ] MainComponent.RingRiskGoesCriticalWhenTheMonitoredSlotsDetectorConfirms (469 ms)
```

That is the mutation check, inverted and for free: adding the guard is the
mutation, and it kills the only two end-to-end proofs that lane R's wiring
works at all.

### Why I did not work around it

Every route out was worse than the gap:

- **Start the engine in the test.** `engine_.start()` opens a real Windows
  audio device. Non-deterministic on this box, absent on the GitHub runner. A
  live-sound suite must not open a device to pass.
- **Guard on the controller's thread or detection state instead.** Same
  collision: tests call `runOnce()` with no detector thread running, so any
  guard keyed on a genuine "is running" flag reads stopped in the fixture. The
  fixture simulates a running detector with no running flag set anywhere, which
  is what makes this class of guard untestable.
- **Add a seam so tests can force the engine-running state.** Explicitly out of
  bounds ("do NOT add a production-visible seam beyond a `const` accessor"), and
  a `const` accessor cannot *set* the state.
- **Delete or rewrite the two failing tests.** They would have to stop asserting
  through the provider, which is the exact end-to-end coverage lane R was built
  to add. Trading the wiring proof for the guard is a net loss.
- **Ship the `lastDataMs_` / sequence-number staleness gate instead.** This is
  the technically right fix — it covers the stopped rig *and* the alive-but-
  silent tap, needs no `NotchController` change, breaks no existing test, and is
  fully drivable headlessly (tick N times without pumping). But the ruling parks
  it as a named follow-up owner decision, and unilaterally building it on a
  live-sound tool exceeds this wave's mandate. **Recommend un-parking it** — it
  is strictly better than the guard that was ruled.

The guard was reverted (`git checkout -- src/app/MainComponent.cpp`);
`src/app/MainComponent.cpp` is byte-identical to `ac3195f` in this wave.

### Docs, written to the measured truth

- `docs/spec-ring-risk.md` §3 — the bullet claiming a stale `Critical` can never
  show is narrowed to what it really guarantees (`Unavailable` overrides the
  hold *when a snapshot is still being published*), followed by the two cases
  that escape it: (a) rig stopped/restarting, **still open**, with the reason
  the provider guard cannot ship against the current fixture, dated and marked
  measured-not-predicted; (b) tap alive, no blocks, detector running.
- `docs/spec-ring-risk.md` "Known gaps (owner)" — new first entry naming the
  `lastDataMs_`/sequence-number timeout gate as the follow-up that covers both,
  marked owner decision.
- `docs/KY-THUAT-CHONG-HU.md` §3.6 — mirrored as a "Khoảng trống đã biết (chưa
  vá)" paragraph after the hold rule.
- `docs/release-notes/1.1.3-alpha.md` — new item 5 under "Việc tester cần
  nhìn": don't read the chip while the rig is stopped, it freezes on its last
  reading rather than going N/A.
- `docs/GIOI-THIEU.md` — one sentence, same point.

## I-4 — plan checkboxes + no stale "pending" claims · DONE

`docs/superpowers/plans/2026-08-27-next-wave.md`: all ten `- [ ]` under Task R1
(237, 240, 241, 246), Task R2 (264, 268) and Task R3 (276, 278, 281, 284) ticked
to `- [x]`. Re-grepping lines 221-290 for an unticked box returns `0`. T2 (325,
327, 331) and C3 (377) human items untouched.

Roadmap status table: unchanged, correctly — lane R lives in the next-wave plan,
not the v2 roadmap.

Straggler sweep, `grep -rn "chưa nối|N/A" docs/ installer/ --include=*.md`:

- `docs/GIOI-THIEU.md:80` and `docs/KY-THUAT-CHONG-HU.md:251,257,259` already
  described the wired behaviour — these are band definitions ("!valid → N/A"),
  not pending claims. Correct as-is.
- `docs/superpowers/plans/2026-08-27-next-wave.md:43` "chưa nối" is about the
  preset lane, not RING RISK. Left alone.
- `installer/TESTER-NOTES.md:17` was the one real straggler — fixed under I-2.

No doc now lists RING RISK as pending or unwired.

## Minors · DONE

- `memory/ring-risk-lane-r-2026-09-06.md:7` — "Suite 451 → 453" → "451 → 454"
  (branch end, confirmed by the ctest tail below). Same header's amendment
  citation moved to the plan (I-1).
- Same file, new section 9: a provider must also answer for the STOPPED state —
  snapshots freeze when no block arrives, so "translate the last number I read"
  is not enough; plus the measured reason the engine-state guard cannot ship and
  the process lesson (a ruling that adds a guard must be checked against the
  fixture before it is written into a spec).
- CRITICAL exception list gained the persistence streak ("chưa đủ số frame liên
  tiếp") in `docs/GIOI-THIEU.md`, `docs/release-notes/1.1.3-alpha.md` and
  `installer/TESTER-NOTES.md`.

---

## Commands and output

Build (incremental Release; the comment-only header change, plus the guard build
before it that produced the I-3 evidence):

```
$ cmake --build build --config Release
  HandsFree.vcxproj -> ...\Release\AZ Soundtech Hands-free.exe
  HandsFreeTests.vcxproj -> ...\build\tests\Release\HandsFreeTests.exe
```

One pre-existing warning, unrelated: C4996 `juce::Displays::Display::userArea`
at `MainComponent.cpp:1296`.

Focused, final state:

```
$ ./build/tests/Release/HandsFreeTests.exe --gtest_filter=*RingRisk*
[==========] 23 tests from 3 test suites ran. (1150 ms total)
[  PASSED  ] 23 tests.
```

23, not 24: no new test was added, because the guard it would have covered is
not in the tree.

Full suite:

```
$ cd build && ctest -C Release
454/454 Test #454: logstats_fixture ... Passed    0.17 sec

100% tests passed, 0 tests failed out of 454

Total Test time (real) =  35.54 sec
```

Baseline 454/454 held.

## Files changed

| File | Finding |
|---|---|
| `src/gui/SpectrumView.h` | I-1 |
| `installer/TESTER-NOTES.md` | I-2, minor |
| `docs/spec-ring-risk.md` | I-1, I-3 |
| `docs/KY-THUAT-CHONG-HU.md` | I-3 |
| `docs/GIOI-THIEU.md` | I-3, minor |
| `docs/release-notes/1.1.3-alpha.md` | I-3, minor |
| `docs/superpowers/plans/2026-08-27-next-wave.md` | I-4 |
| `memory/ring-risk-lane-r-2026-09-06.md` | I-1, minors |

`src/app/MainComponent.cpp` and `tests/test_gui_wiring.cpp` are **unchanged** —
see I-3.

Housekeeping: `.superpowers/sdd/.gitignore` was already absent (checked, nothing
to delete). `progress.md` and the `review-*.diff` files are left unstaged.
`git diff` over `docs/ installer/ memory/` greps clean for mojibake markers.

## Open for the controller

1. **Un-park the `lastDataMs_` / sequence-number staleness gate?** It fixes both
   the stopped rig and the dead tap, is headlessly testable, and touches no DSP.
   It is the only route to closing I-3 that does not cost a green wiring test.
2. **`installer/TESTER-NOTES.md` header** still says v1.0.5 / 368 tests / the
   1.0.5 SHA. Refreshed by the release step, not by me.
