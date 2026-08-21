# Task 10 report — Detector thread FFT pipeline

- **Model used:** claude-opus-5 (Claude Code main loop, acting as its own implementer)
- **Tier escalation:** N/A. The previous session's `implement` subagent channel
  (opencode/deepseek-v4-flash-free) returned empty 4 consecutive times. Rather
  than re-dispatch into a channel with no evidence of recovery, this session
  implemented the task in the main loop under TDD.
- **Status:** DONE_WITH_CONCERNS (see Deviations and Concerns)

## Commits

- `de21d98` fix: generate JuceHeader.h so the app target builds from a clean configure
- `fe126cb` feat: add detector thread FFT pipeline with tests

Branch: `feat/task-10-detector-fft` (not merged to main).

## Files

| File | Delta |
|---|---|
| `src/dsp/Detector.h` | +113 (new) |
| `src/dsp/Detector.cpp` | +70 (new) |
| `tests/test_detector.cpp` | +154 (new) |
| `tests/CMakeLists.txt` | +7 |
| `CMakeLists.txt` | +2 (Task 10) / +5 (JuceHeader fix) |

## Header verification (the brief demanded this before coding)

Grepped `external/JUCE/modules/juce_dsp/frequency/`:

- `FFT (int order);` — juce_FFT.h:58 OK
- `performRealOnlyForwardTransform (float*, bool = false)` — juce_FFT.h:94 OK
- `performFrequencyOnlyForwardTransform (float*, bool = false)` — juce_FFT.h:114 OK
- `WindowingFunction (size_t, WindowingMethod, bool normalise = true, FloatType beta = 0)` — juce_Windowing.h:73 OK
- `multiplyWithWindowingTable (FloatType*, size_t)` — juce_Windowing.h:104 OK
- `WindowingMethod::hann` — juce_Windowing.h:58 OK

The API *names* in the v2 brief are all correct. Three other things in the
brief were not.

## Deviations from the brief (all deliberate, all verified)

### 1. `fftBuffer_` is 2 * kFftSize, not kFftSize — the brief would have overrun

juce_FFT.h documents, for both forward entry points, that the size of the array
passed in must be 2 * getSize(). The brief's `timeBuffer_` was `size kFftSize`
(1024 floats) and was passed straight to
`performFrequencyOnlyForwardTransform`. That is a 4 KB heap overrun on every
call. `Detector::fftBuffer_` is 2048 floats; the contract is documented at the
declaration so it does not get "optimised" back down later.

### 2. 50% overlap implemented; the brief's hot path had a 1024-sample hop

The brief declares `kHopSize = 512` and then never uses it — its step 1 reads
`kFftSize` samples per call, i.e. a hop of 1024. Plan line 114 is explicit:
"Thread reads 1024 samples from ring buffer (50% overlap -> hop 512)". The
plan outranks the brief, and the handoff's own carry-over rule ("Task 14's
auto-release cadence depends on the 512-sample hop") only makes sense with a
real 512 hop.

Implemented: a `history_` window of kFftSize slides left by the hop actually
read, new samples are appended at the tail, and the whole window is
transformed. Consecutive spectra share half their input, giving a new spectrum
every ~10.7 ms at 48 kHz instead of ~21.3 ms.

`Spectrum::readCount` therefore means **new samples consumed this call
(0..kHopSize)**, not 0..kFftSize. A short hop still surfaces as
`readCount < kHopSize` — nothing is padded up. Tasks 11/14 must compare against
`kHopSize`.

### 3. The brief's Hann test asserted something arithmetically false

Brief test 4 asserted `mag[0] < 0.5 * (mag[1] + mag[2] + mag[3])` for a DC
input. JUCE builds a *symmetric* Hann (`w[i] = 0.5 - 0.5*cos(2*pi*i/(N-1))`,
juce_Windowing.cpp) and `normalise = true` scales it so `sum(w) == N`.
Computing the DFT of exactly that window for N = 1024 gives:

    mag[0] = 1024.00   mag[1] = 512.75   mag[2] = 0.3345   mag[3] = 0.1254

so the brief's assertion is `1024 < 256` — it can never pass. Bin 0 dominates
for a DC input under any window; Hann's DC gain of 0.5 is relative to a
rectangular window, not relative to its own bin 1.

Replaced with a test that actually discriminates: `mag[1]/mag[0]` is 0.5007 for
Hann and exactly 0 for a rectangular window, so
`EXPECT_NEAR(mag[1]/mag[0], 0.5, 0.01)` passes if and only if the window was
applied. It passed on the real JUCE output, which independently confirms both
the window and the 2*N buffer sizing.

### 4. Six tests, not four (27 total, not 25)

Added `EmptyTapProducesNoSpectrum` (pins the documented nullptr contract) and
`SetSampleRateIgnoresNonPositive` (mirrors NotchChain's existing guard and its
test name). Brief tests 1-3 kept, with test 1's "call until
readCount == kFftSize" retargeted to `kHopSize` for the overlap design.

### 5. `Detector.h` includes `<juce_dsp/juce_dsp.h>`, not `<JuceHeader.h>`

`JuceHeader.h` is generated per app target and does not exist for
`HandsFreeTests`. The module header works for both targets.

## Pre-existing defect found and fixed (separate commit `de21d98`)

The `HandsFree` app target **did not build at HEAD**. `main.cpp`,
`MainComponent.h` and `AudioEngine.h` all include `<JuceHeader.h>`, but
`juce_generate_juce_header(HandsFree)` was never in `CMakeLists.txt` —
confirmed with `git log -S juce_generate_juce_header -- CMakeLists.txt`
(no commits). `build-task8-msvc/` contains a JuceHeader.h dated 2026-08-20
13:29 from a local edit that was never committed, which is why the Task 8
report could claim a verified MSVC build while HEAD was broken.

Fixed with the one-line call. Committed separately so the Task 10 commit stays
scoped.

## Test output

    100% tests passed, 0 tests failed out of 27
    Total Test time (real) =   0.39 sec

New: Detector.MagnitudeOfSingleTone, PartialReadIsReportedAsPartial,
EmptyTapProducesNoSpectrum, SetSampleRateUpdates, HannWindowApplied,
SetSampleRateIgnoresNonPositive. Previous 21 unchanged and still green.
Both targets build; `AZ Soundtech Hands-free.exe` links.

## Self-review

- **Hot path allocation-free after construction** — yes. `processLatestBlock`
  calls only `tap.read`, `memmove`, `memcpy`, `std::fill`,
  `multiplyWithWindowingTable` and `performFrequencyOnlyForwardTransform`.
  Every vector is sized in the constructor and never resized. No locks, no
  logging, no `juce::String`.
- **Partial reads surface as partial** — yes, `readCount` carries the hop
  verbatim; `PartialReadIsReportedAsPartial` writes 200 samples and asserts
  `readCount == 200`. No zero-padding of the hop; the sliding window shifts by
  exactly `readCount`.
- **Hann genuinely applied, and the test passes for the right reason** — yes.
  The 0.5007 ratio was predicted analytically from JUCE's own window formula
  before the code existed, and the built binary reproduced it.
- **JUCE 9 headers grepped and names match** — yes, see above. The names
  matched; the buffer-size contract did not.
- **TDD honoured** — tests were written first against a stub returning
  `{nullptr, 0, 0.0}`; 4 of 5 failed with "Expected: (spectrum.magnitudes) !=
  (nullptr), actual: NULL". `SetSampleRateIgnoresNonPositive` was added after
  the plain setter went green, and failed before its guard was written.

## Concerns

1. **`readCount` semantics changed meaning** relative to the brief (max
   kHopSize, not kFftSize). Tasks 11/12/14 briefs must be written against
   `kHopSize`.
2. **No independent reviewer.** Same gap Task 9 has. A whole-branch review
   should re-read `7c1718a`, `0f7d480` and `fe126cb` with fresh eyes.
3. **The zero prefix at startup.** A freshly constructed Detector has a zeroed
   `history_`, so the first block after construction is half silence. This is a
   genuine silent prefix, not a disguised short hop, and it self-heals after two
   hops — but Task 11 should not treat the first one or two spectra as
   trustworthy.
4. **`setSampleRate` does not flush `history_` or the tap.** A device change
   leaves stale samples at the old rate in both. Flushing the tap belongs to
   whoever restarts the device (Task 14), since the Detector is not allowed to
   touch the write side.
5. **Pre-existing warning C4324** (`LockFreeRingBuffer::CachePadded` padded due
   to `alignas(64)`) now also fires from `Detector.cpp`. It is informational and
   describes intended behaviour; silencing it would mean editing Task 4's file,
   which is out of scope here. Task 8's "0 warnings" claim did not hold.
6. **The Detector still owns no thread.** The plan's `start()/stop()/run()` were
   deliberately left out; `processLatestBlock` is the unit under test and the
   thread that drives it belongs with the notch controller (Tasks 13-14).
