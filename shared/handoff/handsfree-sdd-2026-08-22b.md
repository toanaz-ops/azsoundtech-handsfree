# SDD handoff — AZ Soundtech Hands-free (2026-08-22, session 2)

Written 01:19 local. **Supersedes `handsfree-sdd-2026-08-22.md`**, which was
written 00:31 the same day. Read this one; the earlier file's "Open decisions"
have all been decided and several of its numbers were estimates that turned out
wrong.

## Where we are

- Repo: `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree`, branch `main` @ **`de6bb7a`**
- **66/66 tests pass** (was 55/55). Both targets build.
  Verified by running the build and ctest directly, after every change.
- **`origin/main` is up to date, and CI is green.** This is new — see below.
- Working tree clean apart from untracked **`CLAUDE.md`** (see Open decisions).
- Docs added this session:
  - `docs/superpowers/specs/2026-08-22-audio-detector-bridge-design.md` — **DRAFT,
    needs approval before any bridge code is written**
  - `docs/superpowers/audits/2026-08-22-gui-interface-audit.md`
  - `.superpowers/sdd/2026-08-19-az-soundtech-hands-free/owner-decisions.md` —
    **new**, every owner decision with its rejected options
- Ledger: `.superpowers/sdd/2026-08-19-az-soundtech-hands-free/progress.md`,
  now tracked in git

## The headline: CI ran, failed, and is now green

The push was the point, and it paid immediately. First run failed in 27 seconds:

```
CMake Error: Generator "Visual Studio 17 2022"
  could not find any instance of Visual Studio.
```

`windows-latest` is now image **`windows-2025-vs2026`**, which ships Visual
Studio 2026. The workflow pinned a toolchain that is not on the runner. **Nothing
in this repo's code was wrong** — and the local build was green throughout. Only
CI's clean checkout could have found it.

Fixed by dropping `-G` so CMake selects the newest VS present; the step carries a
comment saying why it must not be re-pinned. Second run: **success, 10m48s** —
the first green CI in this project's history. `actions/checkout` bumped v3 → v4
after the run warned v3 is being force-migrated off Node 20.

## Task status

**Done:** 1, 2, 3, 4, 6, 7, 8, 9, 10, 11.
**Open:** 5, 12–32 — but see the next section, because 12's blocker has moved.

## Task 12 is no longer hard-blocked

The previous handoff called Task 12 hard-blocked: the harmonic-aware algorithm
iterates locked notches, that state is private in `AudioEngine`, and there is no
thread-safe read-back path.

Owner decision **D-05** dissolves it. Asked what should happen to preset notches
loaded mid-show, the owner chose *"the detector adopts them"* — which makes the
detector the **sole command author**. A sole author does not read back what it
wrote; it remembers what it commanded. No read-back path is needed.

Two independent facts confirm the model has to live in the detector anyway:
`NotchChain::NotchInfo` cannot carry a clock (auto-release needs `locked_at` and
`last_detected_at`), and the GUI needs those same missing timestamps for Task 22's
"Locked Time" column.

This is written up in the bridge design §3. **It is not yet approved.**

## Owner decisions taken this session

All recorded with their rejected alternatives in `owner-decisions.md`. That file
is new and exists because D-00 (the 2026-08-21 rate-change ruling) survives only
as prose — the options weighed at the time were never written down.

| | Decision | Chosen |
|---|---|---|
| D-01 | Push `main` | Yes — 24 commits (not 22) |
| D-02 | Track `.superpowers/` | Yes |
| D-03 | Delete stale build dirs | Yes — 553 MB (not ~1 GB) |
| D-04 | `shared/` + diagram | Commit both |
| D-05 | Preset notches mid-show | Detector adopts them |
| D-06 | Timers during input dropout | Freeze while the tap is dead |

## Defect fixed — `depthDB` had no path into the filter

The previous handoff said `depthDB` is "stored and applied by nothing". It was
worse than that: **`Biquad::setNotchFilter(freq, Q, sampleRate)` takes no depth
argument at all.** It is the RBJ pure notch, an infinite-depth null. There was
nothing to apply depth *with*. `NotchChain`'s own comment admitted depth was
"a hint (currently unused at the biquad level)".

Added a four-argument form: RBJ peakingEQ driven with negative gain. At the
centre frequency the numerator collapses to `2j·α·A·sin ω₀` and the denominator
to `2j·(α/A)·sin ω₀`, so `|H| = A² = 10^(dB/20)` — the requested depth by
construction. The pure notch is its `dB → −∞` limit, which is why the three-arg
form stays.

**Expected level change**, stated per CLAUDE.md's rule: residual at the notch
frequency rises from ~0 to **0.251** of input at the −12.0 dB every existing
caller passes. This is strictly *less* attenuation than before, never more, so no
frequency can be louder than the previous build. **A human should still confirm
at low volume** — that has not happened.

Also refuses a positive `depthDB`. The peaking form is symmetric, so a positive
gain **boosts** the centre frequency. A sign error in a GUI field, a preset file
or the detector would turn the feedback killer into a feedback amplifier at
exactly the ringing frequency.

Unblocks spec §5.1, Task 26's presets (−18 dB / −10 dB), Task 22's Depth column,
Task 20's depth-as-marker-height, and the preset round-trip D-05 assumes works.

## Test debt closed — spec §9.1

The concurrent ring-buffer test now exists in the repo. Three two-thread tests,
capacities 64 / 100 / 1, monotonic sequence, one equality check catching loss,
duplication, reordering and torn reads at once.

Two things about it worth carrying forward:

- **The count assertion is load-bearing.** Without `EXPECT_EQ(consumed, count)`
  the test is vacuous — the failure flag stays false if the consumer never ran,
  so a ring transporting no data at all would pass.
- **The tests were verified to be able to fail.** A one-item-drop mutation in the
  producer was caught by the capacity-100 case. The other two stayed green under
  the same mutation, and the reason is recorded in the test: a 64-item chunk into
  a 64-slot ring is an all-or-nothing write, so only the non-power-of-two case
  drives the partial-write path.

What it does **not** prove is stated in the file: on x86-64, weakening the memory
orderings would still pass here and fail on ARM.

## Carry-over rules that still bind

Everything in the previous handoff's carry-over section still holds. Additions:

- **`readCount` is now a liveness flag, not a cadence source.** D-06 needs two
  clocks: a wall clock measures a *duration*, `readCount > 0` gates whether that
  duration *counts*. `readCount` is not being promoted — it is being demoted.
- **The naive freeze is wrong, not merely worse.** Advancing the live clock only
  on polls that delivered data halves the clock rate in healthy operation, because
  at a 5 ms poll and a 10.67 ms hop about half of all healthy polls are empty.
- **`NotchChain::setNotch` now rejects positive depth**, in addition to the four
  existing parameter guards.

## What is blocked, and by what

- **All bridge implementation** — waiting on approval of the design doc. Deliberate:
  this project's failure mode is building on unexamined assumptions.
- **Tasks 19, 20, 22, 24, 25** — need the bridge's snapshot mechanism.
- **Tasks 16, 17, 18, 23** — need the 11 `AudioEngine` methods from the Lane C
  audit §3. **These depend on nothing in Lane B and could land immediately.**
- **`AudioEngine` still has no passthrough coverage.** Plan Task 9's specified
  loopback test ("output matches input within 0.01 dB") was never written and
  still is not. It needs the Wave 1 accessors to be testable.

## Open decisions awaiting the project owner

1. **Approve the bridge design.** §10 of the doc lists the three places to push
   back: the "GUI needs no lock-free machinery" principle (§1), the deliberate
   departure from plan Task 13 (§6), and the 250 ms tap-silence constant (§4).
2. **`CLAUDE.md` is untracked and not ignored.** It appeared at 00:51 this
   session. Same argument as D-02.
3. **Land the 11 `AudioEngine` methods now?** Splits the GUI lane in two and
   starts half of it early.
4. **Listen to the depth change at low volume.** Required by CLAUDE.md before
   trusting a DSP change; not yet done.

## Process notes

**The push justified itself in 27 seconds.** A workflow that had never run was
pinned to a toolchain that no longer exists on the runner. Local builds could
never have found it. Whatever else is deferred, do not defer running CI.

**Estimates in handoffs drift.** Three numbers in the previous handoff were off:
22 commits (actually 24), ~1 GB of build dirs (actually 553 MB), and "`depthDB`
applied by nothing" (actually: no depth parameter existed to apply). None
changed a decision, but all three were stated with more confidence than the
evidence supported. Prefer measuring at the moment of writing.

**The vacuous-test trap is easy to walk into.** The first version of the
concurrent test asserted only "no mismatch was seen", which passes when nothing
is seen at all. It looked correct and ran in 30 ms for a claimed million items —
the suspicious runtime is what prompted the second look.
