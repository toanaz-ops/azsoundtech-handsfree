# Parallel execution plan — Tasks 5, 12–32

**Written:** 2026-08-21, after the independent review of Tasks 8–10.
**State at writing:** `main` @ `55c0b12`, 40/40 tests pass, both targets build.
Fix pass 1 in flight on `fix/review-pass-1`.

This plan supersedes nothing. It sits alongside
`2026-08-19-az-soundtech-hands-free.md`, which still defines *what* Tasks 12–32
are. This document is about *what order*, *what can run at the same time*, and
*what is blocking what*.

---

## 0. The constraint that governs everything

Every agent works in the **same working tree**. Two agents on two branches will
overwrite each other's files — `git checkout` moves the ground under whoever
else is reading it. Today this caps real parallelism at **one writer**.

Read-only work (review, audit, design, research) parallelises freely against
that one writer. Writing work does not.

### Lifting the cap: worktrees + a JUCE junction

`git worktree add` gives each writer its own directory. The catch, verified:
`external/JUCE` is a **submodule** (its `.git` is a file, not a directory), so a
fresh worktree gets an **empty** `external/JUCE` and the build dies at
`add_subdirectory`. Re-running `git submodule update --init` per worktree
re-clones ~99 MB plus history — too slow to do repeatedly.

The fix is a directory junction. JUCE is read-only vendored code, so sharing one
checkout across worktrees is safe:

```
git worktree add ../hf-lane-b -b <branch>
cmd //c mklink /J "..\hf-lane-b\external\JUCE" "<repo>\external\JUCE"
```

> **VERIFIED 2026-08-22 — and the two lines above are NOT SUFFICIENT.**
> The full, tested procedure is
> **[`docs/superpowers/runbooks/parallel-lanes.md`](../runbooks/parallel-lanes.md)**.
> Follow that, not this snippet.
>
> The check this section asked for was run on a throwaway lane. Outcome:
> worktree + junction **does** work — clean configure, both targets built,
> 70/70 tests passing inside the lane, `git status` clean, commit and rollback
> fine, main repo unaffected. But it needs a step this plan does not mention,
> and gets one detail wrong:
>
> 1. **The junction alone kills git in the lane** —
>    `fatal: not a git repository: external/JUCE/../../.git/modules/external/JUCE`.
>    The submodule's `.git` file holds a *relative* gitdir that only resolves
>    from the main repo. The lane needs
>    `git config --worktree submodule."external/JUCE".{active=false,ignore=all}`,
>    which in turn needs `extensions.worktreeConfig true` set once in the main
>    repo. This is the "confuses git's gitlink handling" case the paragraph
>    below anticipated; it is fixable rather than fatal.
> 2. **`--worktree` is load-bearing.** Plain `git config` writes to the *shared*
>    config and disabled the submodule in the **main** repo during this very
>    verification. Caught and reverted, but a lane setup that does it silently
>    breaks the main checkout for every other lane.
> 3. **The generator here is `Visual Studio 18 2026`, not 17 2022.** This
>    machine has only Visual Studio Build Tools 2026. `CLAUDE.md`'s documented
>    command fails on a clean configure and appears to work only because
>    `build/` holds a cache recording the real generator.

Junctions were already known to work in this environment — the project auditor
used one to get around a `MAX_PATH` failure during its clean build.

### Build directories

Each lane needs its own, **outside the repo and on a short path**. MSVC's
FileTracker fails with `FTK1011` when the path is long — this has now bitten two
agents. `C:/Users/id_az/AppData/Local/Temp/<lane>` works.

Also: `build-review/`, `build-task8/`, `build-task8-msvc/`, `build-verify/` are
~1 GB of contradictory historical state, and `build-task8-msvc/` still holds the
stale `JuceHeader.h` that let Task 8 claim a verified build over a broken HEAD.
Delete all four. Keep `build/`.

---

## 1. The critical path is not a task in the plan

Tasks 12, 13, 14, 15 and 19 all depend on the same missing thing: **a defined
boundary between the audio thread and the detector thread**. Right now that
boundary is four separate holes:

| Hole | Blocks | Evidence |
|---|---|---|
| No command queue instantiated | 13 | `NotchCommand` appears only in its own header and the CMake source list. Task 5 is not done. **[Correction 2026-08-27: đã ship — AudioEngine sở hữu 8 command queue, drain dưới shared budget 256 (AudioEngine.cpp:465-470).]** |
| No thread-safe read of locked-notch state | 12 | Notch state is private in `AudioEngine`; `NotchChain.h` says all methods are audio-thread-only. |
| No detector thread and no wall clock | 14 | `Detector` has no `start/stop/run`. `Spectrum::readCount` is a *buffer-position* hop; an input dropout advances it by nothing while real time passes. Spec §5.2 steps 6–7 need real elapsed time (~30 ms, 30 s). |
| Magnitudes handed out as a borrowed pointer | 19 | Invalidated on the next `processLatestBlock()`. A 60 fps GUI thread cannot read it without a race. |

Building 12, 13 and 14 on four independent guesses about this boundary is how a
project acquires an architecture nobody chose. **Design it once, deliberately,
before implementing any of them.**

That design pass is read-only. It can start immediately, in parallel with fix
pass 1. It is the single highest-value thing available right now.

---

## 2. The unlock: publish the interface before the implementation

`AudioEngine` currently exposes almost nothing. Plan Tasks 16, 17, 18, 22 and 24
each need accessors that do not exist — device type enumeration, sample-rate and
buffer-size **setters** (only getters exist today), notch state read-back, and
clear-all. Task 19 needs a safe spectrum subscription.

If each GUI task widens the interface as it goes, every GUI task collides with
every other GUI task and with the DSP spine.

**So: land the full public surface of `AudioEngine` and `Detector` early, in one
commit, even where the bodies are still thin.** Once the header is stable and
merged, the GUI, licensing and installer lanes can all be written against it
concurrently without touching each other's files.

This is the difference between four lanes and one.

---

## 3. Wave schedule

### Wave 0 — now, alongside fix pass 1

| Lane | Work | Writes? | Conflicts |
|---|---|---|---|
| **A** | Fix pass 1 (running) | yes — Biquad, NotchChain, AudioEngine, Detector | owns the DSP files |
| **B** | **Design the audio↔detector bridge.** Produce a design doc: command queue shape and drain point, notch-state snapshot mechanism, detector thread + clock source, spectrum publication. Decide, with reasons, and name the alternatives rejected. | no — document only | none |
| **C** | **GUI interface audit.** Read Tasks 16–24 against the current headers; produce the exact list of accessors, setters and subscriptions that must exist, with the plan line requiring each. Feeds Wave 1's interface commit. | no — document only | none |
| **D** | **Infra.** Push `main` (18 commits unpushed, CI has never run once); decide whether `.superpowers/` is tracked (the whole audit trail is currently untracked *and* unpushed); delete the four stale build dirs; commit or ignore `shared/` and `opencode-harness-diagram.html`. | yes — `.gitignore` only | trivial |

Lane D needs the owner's go-ahead: pushing is an outward action.

### Wave 1 — after fix pass 1 merges

Sequential, single writer. This is the critical path and it is not parallelisable.

1. **Bridge implementation** (from Lane B's design): instantiate
   `LockFreeRingBuffer<NotchCommand>` and drain it in the callback — this closes
   Task 5, which the ledger wrongly marked complete; add the notch-state
   snapshot; add the detector thread and its clock; double-buffer or copy out the
   magnitudes.
2. **Interface publication** (from Lane C's audit): the full `AudioEngine` /
   `Detector` public surface in one commit.

Running concurrently in a worktree, because it touches only
`tests/test_ringbuffer.cpp`:

- **Concurrent ring-buffer test.** Spec §9.1 requires it; Task 4 deferred it to
  Tasks 5/8, both of which closed without it. The DSP reviewer already ran a
  40 M-item two-thread torture test (power-of-two *and* non-power-of-two
  capacity) and found the memory ordering correct — this is about bringing that
  evidence *into the repo*, not about discovering something new.
  **[Correction 2026-08-27: đã có trong repo — 3 test concurrent tại
  test_ringbuffer.cpp:342, :350, :368.]**

### Wave 2 — four genuine lanes

Once the interface is merged, these touch disjoint files:

| Lane | Tasks | Owns |
|---|---|---|
| **DSP spine** | 12 → 13 → 14 → 15 | `PeakinessAnalyzer`, `NotchChain`, `AudioEngine` internals, `Detector` internals |
| **GUI** | 16–24 | `src/gui/*`, `MainComponent` |
| **Licensing** | 27–29 | new files only |
| **Installer** | 30–32 | `installer/*` |

The DSP spine stays sequential internally — 13 consumes 12's candidates, 14
consumes 13's notch state, 15 changes 14's cadence.

Two lanes need a correction carried in before they start:

- **Installer:** plan Task 30's NSIS script has the wrong path *and* the wrong
  filename. JUCE emits
  `build\HandsFree_artefacts\Release\AZ Soundtech Hands-free.exe`, verified in a
  clean build — not `build\Release\HandsFree.exe`.
- **GUI:** `depthDB` is stored by `NotchChain` and applied by nothing, so every
  notch is a full-depth null. Spec §5.1 requires 6–24 dB and plan Task 26's
  presets ("Speech" −18 dB, "Music" −10 dB) cannot be represented. Task 22's
  notch-list Depth column would display a number with no effect. Fix the
  coefficient math before the GUI displays it.

---

## 4. Test debt that must not wait for a convenient moment

These are not tasks in the plan and will therefore never happen unless
scheduled. Each closes a hole the current 40 green tests do not see.

1. **`AudioEngine` has zero automated coverage.** `tests/CMakeLists.txt`
   compiles Biquad, NotchChain, Detector and PeakinessAnalyzer only. The whole
   audio I/O layer — device lifecycle, the passthrough contract, null-channel
   guards, the bypass path, the tap write — is untested. Plan Task 9's specified
   loopback test ("verify output matches input within 0.01 dB") was never
   written. Schedule it against the Wave 1 interface, once the accessors exist to
   make it testable.
2. **The ledger's per-task test counts are cumulative suite totals.** Task 9
   records "21/21 PASS" while nothing anywhere instantiates `AudioEngine`. Every
   future task report must state *which* tests are new and what production change
   would make each fail — not the suite total.

---

## 5. What must not be parallelised, and why

- **Tasks 12, 13, 14, 15** — a chain of data dependencies, not four tasks.
- **Anything touching `AudioEngine.cpp` at the same time as the bridge work.**
  It is the busiest file in the repo and the one every lane wants.
- **Two writers without worktrees.** Restating it because it is the failure mode
  that silently destroys work rather than announcing itself.

---

## 6. Immediate next actions

1. Start Lane B (bridge design) and Lane C (GUI interface audit) now — both
   read-only, both unblock Wave 1.
2. Get the owner's decision on Lane D (push / `.superpowers` tracking / build dir
   cleanup).
3. Verify the worktree + JUCE junction approach once, on a throwaway lane, before
   Wave 2 depends on it.
4. When fix pass 1 lands, verify it independently — build and run ctest directly
   rather than accepting the report. This project has a documented history of
   green claims that were false.
