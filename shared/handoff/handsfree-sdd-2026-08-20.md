# SDD handoff — AZ Soundtech Hands-free (2026-08-20)

## Where we are

- Repo: `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree`
- Plan: `docs/superpowers/plans/2026-08-19-az-soundtech-hands-free.md`
- Spec: `docs/superpowers/specs/2026-08-19-az-soundtech-hands-free-design.md`
- SDD ledger: `.superpowers/sdd/2026-08-19-az-soundtech-hands-free/progress.md`
- HEAD: `0f7d48047cb91160cd89eb32e37995bb96a5e47e` (post-notch tap landed)
- Working tree clean apart from pre-existing `opencode-harness-diagram.html`.

## Harness shape (verified by reading the file)

- Orchestrator: `shopaikey-provider/claude-opus-5` (Plan agent, `edit: deny`)
- Implementer: `opencode/deepseek-v4-flash-free` (Implement agent)
- Tier 2 escalation: `shopaikey-provider/claude-sonnet-5` medium variant — for
  complex DSP math / new architecture / multi-system integration / Tier-1 failure
- 21/21 tests currently pass on MSVC Release (verified by orchestrator run, not
  trust). 18 from the original DSP + 3 from Task 9 sample-rate retarget.

## Tasks done

1, 2, 3, 4 (individually), 5–7 (combined), 8, 9. Commit history in
`.superpowers/sdd/.../progress.md`.

## Tasks open

10 (FFT pipeline), 11 (peakiness), 12 (harmonic-aware), 13–15 (notch control),
16–24 (GUI), 25–26 (presets), 27–29 (license), 30–32 (installer/integration).

## Carry-over rules that bind Task 10 onwards

- Tap from `AudioEngine::getTapBuffer()` is gated on input channel 0 being
  non-null. The 512-sample hop is a buffer-position hop, not a wall-clock hop.
  Partial `read()` (< 1024) must surface in `Spectrum::readCount`. Do NOT
  zero-pad above the read count and call it a full block. Tasks 11 and 14
  rely on this.
- `NotchChain::setSampleRate(double)` exists and recomputes Active notches
  from stored `NotchInfo`. AudioEngine already wires it in
  `audioDeviceAboutToStart`.
- Detector must be the sole reader of the tap (SPSC discipline).
- All audio-thread code (AudioEngine callback, NotchChain, Biquad) is real-time:
  no allocation, no locks, no logging, no `juce::String` on the callback path.

## Two blockers I (the orchestrator) want to flag

1. The `implement` subagent has returned empty 4 consecutive times in this run
   (Task 10 implement + 3 review retries for Task 9, plus the Task 10 probe
   retry). The Task 9 implementer DID land work despite the empty return — its
   commits `7c1718a` and `0f7d480` are in HEAD. The Task 10 implementer did
   NOT land work; HEAD is unchanged. I do not have evidence that re-dispatching
   the same prompt into `implement` will start working. Switching subagent type
   (e.g. to `general`) might help; so might Tier-2 escalation to Sonnet 5.

2. The v1 Task 10 brief was wrong about JUCE 9 FFT API names. v2 (this folder
   `task-10-brief.md`) names the actual APIs after grepping the vendored
   headers under `external/JUCE/modules/juce_dsp/frequency/`:
   - `juce::dsp::FFT::performRealOnlyForwardTransform(float*, bool)` (not `performRealOnlyForward`)
   - `juce::dsp::FFT::performFrequencyOnlyForwardTransform(float*, bool)` (magnitude-only shortcut)
   - `juce::dsp::WindowingFunction<float>(size_t, WindowingMethod, bool normalise = true, FloatType beta = 0)`
   - `multiplyWithWindowingTable(samples, size)` (not `multiplyWithWindow`)
   - `WindowingMethod::hann`
   The v2 brief instructs the implementer to grep these headers themselves
   before coding, and to stop and write a probe report if anything differs.

## What I have NOT done and want a future session to consider

- Resuming Task 10 with the v2 brief and a fresh subagent channel.
- Independently re-reading commits `7c1718a` and `0f7d480` (Task 9) — that task
  was reviewed by the orchestrator, not an independent reviewer, because the
  review channel returned empty three times in a row.
- Tier-2 escalation (Sonnet 5 medium) for any DSP task that Tier 1 botches.
