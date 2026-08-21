# SDD handoff — AZ Soundtech Hands-free (2026-08-22)

Supersedes `handsfree-sdd-2026-08-21.md` and `handsfree-sdd-2026-08-20.md`.
Read this one. The two older files contain claims this session disproved.

## Where we are

- Repo: `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree`, branch `main` @ **`3fa4138`**
- **55/55 tests pass** on MSVC Release; both targets build; the app exe links.
  Verified this session by building and running ctest directly after every
  merge — not taken from any agent's report.
- Working tree clean apart from untracked `shared/` and
  `opencode-harness-diagram.html` (see Open decisions).
- Plans: `docs/superpowers/plans/2026-08-19-az-soundtech-hands-free.md` (what the
  tasks are) and **`docs/superpowers/plans/2026-08-21-parallel-execution-plan.md`**
  (what order, what can run concurrently, what blocks what). Read the second one
  before scheduling anything.
- Spec: `docs/superpowers/specs/2026-08-19-az-soundtech-hands-free-design.md`
  — amended this session, see "Spec changes" below.
- Ledger: `.superpowers/sdd/2026-08-19-az-soundtech-hands-free/progress.md`

## Task status — corrected

**Done:** 1, 2, 3, 4, 6, 7, 8, 9, 10, 11.

**Task 5 is NOT done, despite what the old ledger said.** `src/dsp/NotchCommand.h`
defines the struct and nothing else in the repo references it — verified by
`grep -rn NotchCommand src/ tests/ CMakeLists.txt`, which finds it only in its own
header and the CMake source list. The plan requires a queue "using
`LockFreeRingBuffer<NotchCommand>`"; no such queue is instantiated anywhere.
`tasks-5-7-report.md` claims the struct "is exercised by the existing
LockFreeRingBuffer tests" — false, those tests instantiate
`LockFreeRingBuffer<int>`. Treat Task 5 as open.

**Open:** 5, 12–32.

## What this session did

Four merges to `main`, all fast-forward, all independently verified:

| Commit | What |
|---|---|
| `de21d98` | fix: generate JuceHeader.h so the app target builds from a clean configure |
| `fe126cb` | feat: Task 10 — detector FFT pipeline |
| `e65ddfc` | feat: Task 11 — peakiness scoring |
| `55c0b12` | fix: peakiness annulus excluding the Hann main lobe |
| `ff2063e` | fix: notch coefficient guards + denormal suppression |
| `3d1bc68` | fix: tap accessor defined, tap drained on restart, drops counted |
| `3fa4138` | docs: parallel execution plan |

Plus two independent reviews (DSP/real-time, and spec-compliance/build-integrity)
whose findings are recorded in the ledger.

Stale merged branches still present: `feat/task-10-detector-fft`,
`feat/task-11-peakiness`, `fix/review-pass-1`. All fully merged; safe to delete.

## Defects fixed this session (all were live in `main`)

Measured before → after:

- **Denormal limit cycle.** No `juce::ScopedNoDenormals` existed anywhere. After
  ~4 s of digital silence the notch chain's state fell into the subnormal range
  and never left: **77 ms per second of audio → 0.9 ms**, ~80x. Reachable by
  muting a mic after soundcheck.
- **Divergent filter on rate change.** `Biquad::setNotchFilter` validated
  nothing. Pole radius is `sqrt((1-alpha)/(1+alpha))`; `freq >= sampleRate/2`
  makes `alpha` negative and the radius exceed 1. Notch at 30 kHz set legally at
  96 kHz, then retargeted to 44.1 kHz: peak output **4.11e18 → 1.0**.
- **`AudioEngine::getTapBuffer()` was declared and never defined.** Task 9 built
  the tap, Task 10 built its consumer, both were marked complete, and they could
  not be connected — the LNK2019 was reproduced verbatim before the fix.

## Spec changes made this session

Both are surgical edits at the source, so nobody re-derives them:

1. **§5.2 step 4 — peakiness formula corrected.** The spec's
   `mean(neighbors ±2 bins)` is broken for this pipeline. Task 10 applies a Hann
   window and a Hann main lobe is **four bins wide** — bin `i±1` carries ~0.50 of
   the peak, so the ±2 neighbourhood measures the tone against itself and
   peakiness has a hard ceiling of **4.0**. Measured at the old radius: on-bin
   tone 3.99, 1 kHz tone 3.29, noise up to 3.46 — noise outscored the tone and
   the threshold of 10.0 could never fire. Now an **annulus at ±3, ±4, ±5**,
   excluding offsets 0, ±1, ±2. 1 kHz tone scores 131.7; worst noise-only bin
   over 60 seeds is 7.35; zero false candidates. **The threshold 10.0 is
   unchanged — the radius was the defect, not the constant.**

2. **§8 line 182 — rate-change behaviour.** The spec said "clear notch,
   re-init"; Task 9 implemented the opposite and a green test locked it in. The
   project owner ruled on 2026-08-21: **keep the notches, guard Nyquist.** A
   notch survives a rate change; a notch whose frequency is no longer below the
   new Nyquist goes Idle retaining its stored parameters.

## Known v1 limitation — the detector is blind below ~234 Hz

The peakiness annulus needs five bins of headroom on both sides, so the lowest
scoreable bin is 5 = **234 Hz at 48 kHz** (~215 Hz at 44.1 kHz, ~469 Hz at
96 kHz). `kDefaultMinFrequencyHz = 100.0` is therefore never the binding
constraint. Low-mid feedback at 200–250 Hz is real in live sound, so this is a
product gap, not a footnote. Documented in `src/dsp/PeakinessAnalyzer.h`.

Narrowing the annulus to ±3..±4 would lower the floor to 188 Hz but a noise bin
reaches **10.37** over 60 seeds — an outright false positive at the default
threshold. A one-sided annulus near the low edge was considered and rejected:
on the rising low-frequency noise floor typical of live sound it would
underestimate the local background and inflate peakiness, producing more false
positives exactly where the product's headline goal is fewer.

Changing this is a one-constant edit. Re-run the multi-seed noise sweep if you do.

## Carry-over rules that bind the next tasks

- **`Detector::Spectrum::readCount` ranges 0..kHopSize (512), not 0..kFftSize.**
  Any brief saying otherwise is wrong.
- **`readCount` is a buffer-position hop, not wall-clock.** `AudioEngine` gates
  the tap write on input channel 0 being non-null, so an input dropout advances
  it by nothing while real time passes. Spec §5.2 steps 6–7 need real elapsed
  time (~30 ms, 30 s) — **do not build a cadence on `readCount`.**
- `processLatestBlock` returns `magnitudes == nullptr` when the tap had nothing
  new. Handle before dereferencing.
- A freshly constructed `Detector` has a zeroed history, so the first one or two
  spectra carry a genuine silent prefix. Do not score them.
- `Spectrum::magnitudes` is a **borrowed pointer**, invalidated on the next
  `processLatestBlock()`. Any cross-frame comparison must copy.
- All audio-thread code is real-time: no allocation, no locks, no logging, no
  `juce::String`. `Biquad::setNotchFilter` is now reachable from the audio thread
  via `NotchChain::setNotch` and is branch-only by design — keep it that way.
- `AudioEngine.h` and `Detector.h` include JUCE **by module**
  (`<juce_audio_devices/...>`, `<juce_dsp/...>`), not `<JuceHeader.h>`. That is
  what lets them compile into the test target. Keep it that way; `JuceHeader.h`
  only exists for the app target.

## What is blocked, and by what

**Task 12 is hard-blocked.** The harmonic-aware algorithm iterates locked
notches. That state lives in `AudioEngine::notchChains_`, a private member with
no accessor, and `NotchChain.h` states all methods are audio-thread-only. There
is no thread-safe read-back path from notch state to the detector thread. This
must be designed before Task 12 can be written as specified.

**Tasks 12, 13, 14, 15 and 19 all depend on the same missing thing:** a defined
boundary between the audio thread and the detector thread. Today that is four
holes — no command queue instantiated, no notch-state read-back, no detector
thread and no clock, and magnitudes handed out as a borrowed pointer. The
parallel execution plan argues this should be designed **once**, deliberately,
before any of the four are implemented. Building them on independent guesses is
how a project acquires an architecture nobody chose.

**Tasks 16, 17, 18, 22, 24** each need `AudioEngine` accessors that do not
exist — Task 17 needs sample-rate and buffer-size **setters** and only getters
exist. The plan's recommendation: land the whole public surface in one commit
before the GUI lane starts, so GUI/licensing/installer can run concurrently
without colliding.

**Task 30's NSIS path is wrong** in the plan, both directory and filename. JUCE
emits `build\HandsFree_artefacts\Release\AZ Soundtech Hands-free.exe`, verified
in a clean build — not `build\Release\HandsFree.exe`.

**`depthDB` is stored and applied by nothing**, so every notch is a full-depth
null. Spec §5.1 requires 6–24 dB; Task 26's presets (Speech −18 dB, Music
−10 dB) cannot be represented; Task 22's Depth column would display a number
with no effect. Fix the coefficient math before the GUI displays it.

## Test debt that will not happen unless scheduled

- **Plan Task 9's loopback test was never written** ("verify output matches input
  within 0.01 dB"). `AudioEngine` had zero coverage until this session's fix pass
  added four tests; the passthrough contract is still untested.
- **Spec §9.1's concurrent ring-buffer test still does not exist in the repo.**
  Task 4 deferred it to Tasks 5/8; both closed without it. The DSP reviewer ran a
  40 M-item two-thread torture test (power-of-two and non-power-of-two capacity)
  and found the memory ordering correct — that evidence lives in a session
  transcript, not in the repo.

## Open decisions awaiting the project owner

None of these were actioned; all were raised and are unanswered.

1. **Push `main` to remote.** `origin/main` is at `cabe715` — **22 commits
   behind**. The CI in `.github/workflows/build.yml` exists, looks correct, and
   **has never run once**. It would have caught the missing
   `juce_generate_juce_header` back at Task 3 instead of letting it survive to
   Task 10. Pushing is an outward action and needs an explicit yes.
2. **Whether `.superpowers/` should be tracked.** `.gitignore` line 59 excludes
   it, so `progress.md` and all task reports — the project's entire audit trail —
   are untracked *and* unpushed. One disk failure erases both the history and the
   record of it.
3. **`shared/` and `opencode-harness-diagram.html`** are untracked and not
   ignored. Commit them or add ignore rules.
4. **Delete the four stale build directories** (`build-review/`, `build-task8/`,
   `build-task8-msvc/`, `build-verify/`, ~1 GB). `build-task8-msvc/` still holds
   the stale `JuceHeader.h` that let Task 8 claim a verified build over a broken
   HEAD. Keep `build/`.
5. **Start Lanes B and C** from the parallel execution plan — design the
   audio↔detector bridge, and audit the GUI interface requirements. Both are
   read-only and unblock Wave 1.

## Process notes worth carrying forward

**Verify agent reports by execution, never by reading them.** This session found
that Task 8's "build verified on MSVC, 0 warnings" was false — the app target
could not compile from a clean configure at all, and the evidence lived in an
uncommitted build directory. Every merge this session was preceded by the
orchestrator running the build and ctest personally.

**Per-task test counts in the ledger are cumulative suite totals, not per-task
evidence.** Task 9 records "21/21 PASS" while nothing anywhere instantiated
`AudioEngine`. Future task reports should state which tests are new and what
production change would make each one fail.

**Tell implementers the brief may be wrong.** Every brief this session carried
the line "spec > plan > brief; if this contradicts them, stop and report rather
than implement what you believe is wrong." Four consecutive agents used that
permission and each overturned at least one orchestrator claim — including three
numeric errors that would otherwise have been written into test comments and
become "facts" for the next reader. The one that mattered most: an implementer
refused to substitute a working constant for a broken spec formula, and instead
committed a default it knew could not fire, with the reason. That is how the
Hann main-lobe defect surfaced at all.

Related memory notes: `memory/build-verification-2026-08-21.md`,
`memory/brief-verification-2026-08-21.md`.
