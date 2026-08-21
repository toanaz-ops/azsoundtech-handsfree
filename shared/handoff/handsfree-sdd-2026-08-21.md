# SDD handoff — AZ Soundtech Hands-free (2026-08-21)

Supersedes `handsfree-sdd-2026-08-20.md`. Read this one.

## Where we are

- Repo: `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree`
- Plan: `docs/superpowers/plans/2026-08-19-az-soundtech-hands-free.md`
- Spec: `docs/superpowers/specs/2026-08-19-az-soundtech-hands-free-design.md`
- SDD ledger: `.superpowers/sdd/2026-08-19-az-soundtech-hands-free/progress.md`
- Branch: **`feat/task-10-detector-fft`**, HEAD `90b9929`. NOT merged to `main`
  (`main` is still at `0f7d480`). Merging is the first decision for the next
  session.
- Working tree clean apart from the pre-existing untracked
  `opencode-harness-diagram.html` and this `shared/` folder.
- **27/27 tests pass** on MSVC Release, and **both** targets build — verified by
  running the build and ctest directly this session, not taken from a report.

## Tasks done

1, 2, 3, 4, 5-7, 8, 9, **10**.

## Tasks open

11 (peakiness), 12 (harmonic-aware), 13-15 (notch control), 16-24 (GUI),
25-26 (presets), 27-29 (license), 30-32 (installer/integration).

## What changed this session

Task 10 landed. It was implemented **in the main loop by claude-opus-5 under
TDD**, not by a subagent — the previous session's `implement` channel had
returned empty 4 consecutive times and there was no evidence a 5th dispatch
would behave differently. This worked; consider it the default for DSP tasks
until the subagent channel is shown to be healthy again.

Commits on the branch:

- `de21d98` fix: generate JuceHeader.h so the app target builds from a clean configure
- `fe126cb` feat: add detector thread FFT pipeline with tests
- `90b9929` docs: record build- and brief-verification lessons from Task 10

## The v2 Task 10 brief had three more errors

Detail in `task-10-report.md`. Summary, because it changes downstream briefs:

1. **Buffer overrun.** `juce_FFT.h` requires the array passed to both forward
   entry points to be `2 * getSize()` floats. The brief sized it `kFftSize`.
   Implemented as `fftBuffer_` of `2 * kFftSize`.
2. **Hop was wrong.** The brief declared `kHopSize = 512` and then read
   `kFftSize` per call (a 1024 hop). Plan line 114 requires 50% overlap. Real
   overlap is now implemented via a sliding `history_` window.
3. **A test that could never pass.** The brief's Hann assertion was
   `1024 < 256` in disguise. Replaced with the `mag[1]/mag[0] ~= 0.5` ratio
   test, which a rectangular window fails.

Lesson recorded in `memory/brief-verification-2026-08-21.md`: verify the API
names, the *semantic contract in the docstring*, and the *arithmetic of every
assertion* — the v2 brief had only done the first.

## The app target was broken at HEAD and nobody noticed

`juce_generate_juce_header(HandsFree)` was never in `CMakeLists.txt`
(`git log -S` finds no commit adding it), so every `#include <JuceHeader.h>`
failed from a clean configure. `build-task8-msvc/` contained an uncommitted
generated header, which is how Task 8 reported a verified MSVC build over a
broken HEAD. Fixed in `de21d98`.

Lesson in `memory/build-verification-2026-08-21.md`. Practical consequence:
**there are five build dirs** (`build/`, `build-review/`, `build-task8/`,
`build-task8-msvc/`, `build-verify/`) holding contradictory historical state.
Use `build/` and consider deleting the rest.

## Carry-over rules that bind Task 11 onwards

- **`Detector::Spectrum::readCount` ranges 0..kHopSize (512), NOT 0..kFftSize.**
  This is the single most important carry-over. Any brief that says
  `readCount == kFftSize` is wrong. A value below `kHopSize` means the tap
  under-ran and the analysis timeline advanced by less than one hop.
- `processLatestBlock` returns `magnitudes == nullptr` when the tap had nothing
  new. Task 11 must handle that before dereferencing.
- A freshly constructed `Detector` has a zeroed `history_`, so the first one or
  two spectra carry a genuine silent prefix. Not a bug, but do not score them.
- `setSampleRate` does not flush `history_` or the tap. Flushing the tap on a
  device change belongs to Task 14, since the Detector must never touch the
  write side (SPSC).
- The Detector owns no thread. The plan's `start()/stop()/run()` were left out
  on purpose so the FFT stage is unit-testable without an audio device; the
  driving thread belongs with the notch controller (Tasks 13-14).
- Tap gating from Task 9 still applies: `AudioEngine` only writes the tap when
  input channel 0 is non-null, so a hop is a buffer-position hop, not wall-clock.
- All audio-thread code (AudioEngine callback, NotchChain, Biquad) is real-time:
  no allocation, no locks, no logging, no `juce::String` on the callback path.
- `Detector.h` includes `<juce_dsp/juce_dsp.h>`, not `<JuceHeader.h>`, so it
  compiles into both the app target and `HandsFreeTests`. Keep it that way.

## Open items for the next session

1. **Merge `feat/task-10-detector-fft` into `main`** (or open the PR the
   project's own convention in `memory/MEMORY.md` calls for — note that Tasks
   1-9 were committed straight to `main`, so the convention has not actually
   been followed).
2. **Independent review of Tasks 9 AND 10.** Neither got a fresh reviewer.
   Commits to re-read: `7c1718a`, `0f7d480`, `fe126cb`.
3. **Task 11 (peakiness)**, written against `kHopSize` semantics.
4. Pre-existing warning C4324 on `LockFreeRingBuffer::CachePadded` fires from
   every translation unit that includes it. It describes intended `alignas(64)`
   padding. Silence it at the struct or accept it — but stop reporting
   "0 warnings" until one of those happens.
