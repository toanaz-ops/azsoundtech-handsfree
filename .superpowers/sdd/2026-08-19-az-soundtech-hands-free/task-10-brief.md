# Task 10: Detector thread — FFT pipeline (REVISED)

## Revision history

- v2: corrected JUCE 9 FFT API names and WindowingFunction signature against the
  vendored headers under `external/JUCE/modules/juce_dsp/frequency/`. The v1 brief
  named `performRealOnlyForward`, which does not exist in JUCE 9.0.1. v2 names the
  real APIs as they actually appear.

## Confirm before you write code

The implementer MUST grep the vendored headers and confirm these signatures before
writing any code. If the signatures differ, the brief is wrong again and you stop
and write only a probe report.

- `juce::dsp::FFT` is in `external/JUCE/modules/juce_dsp/frequency/juce_FFT.h`.
- Constructor: `FFT (int order);` — order-10 = 1024-point FFT.
- Real-input forward (returns complex spectrum in-place; first half of `inOutData`
  is the spectrum, only non-negative frequencies fit when called with the
  `true` flag):
  `void performRealOnlyForwardTransform (float* inputOutputData, bool onlyCalculateNonNegativeFrequencies) const noexcept;`
- Magnitude-only forward (no phase, no complex output — useful alternative):
  `void performFrequencyOnlyForwardTransform (float* inputOutputData, bool onlyCalculateNonNegativeFrequencies) const noexcept;`
  Result for N=1024 is 513 bins (non-negative frequencies), interleaved as
  `(DC, mag[f1], mag[f2], ..., mag[fN/2])` for `true`; with `false`, a full N
  bins including the negative-frequency mirror.
- `juce::dsp::WindowingFunction<float>` is in
  `external/JUCE/modules/juce_dsp/frequency/juce_Windowing.h`.
- Constructor: `WindowingFunction (size_t size, WindowingMethod, bool normalise = true, FloatType beta = 0);`
- Apply: `void multiplyWithWindowingTable (FloatType* samples, size_t size) const noexcept;`
- `WindowingMethod::hann` is the enum value to use.

## Why we picked performFrequencyOnlyForwardTransform

We do not need complex output — Tasks 11 and 12 score on magnitudes only. The
magnitude-only entry point:
- produces 513 magnitudes directly (no interleaved complex to split),
- halves the work after the FFT,
- removes any "did we compute magnitude correctly" risk in the orchestrator's
  eye (no sqrt(re²+im²) to get wrong).

Use it. Document the choice in `Detector.h`.

## Carry-over rule from Task 9

The tap write in `AudioEngine` is gated on input channel 0 being non-null.
If the input drops or the device returns null input pointers, the tap stops
getting samples while the detector keeps reading. The 512-sample hop is
therefore a BUFFER-POSITION hop, not a wall-clock hop. A partial `read()`
(< 1024) means the FFT block is not a clean hop of contiguous signal —
record this in `Spectrum::readCount` and surface it to Tasks 11 and 14. Do
NOT pad with zeros and pretend it is a full block.

## Files

- Create: `src/dsp/Detector.h`
- Create: `src/dsp/Detector.cpp`
- Create: `tests/test_detector.cpp`
- Modify: `tests/CMakeLists.txt` to add `test_detector.cpp` to `HandsFreeTests`
- Modify: `CMakeLists.txt` (root) to add `src/dsp/Detector.{h,cpp}` to the
  `HandsFree` target sources

## Class interface (exact)

```cpp
class Detector
{
public:
    static constexpr int kFftSize = 1024;
    static constexpr int kHopSize = 512;
    static constexpr int kNumBins = kFftSize / 2 + 1;   // 513

    struct Spectrum
    {
        const float* magnitudes = nullptr;  // kNumBins entries, valid until next call
        size_t        readCount  = 0;       // samples actually read for this block
        double        sampleRate = 0.0;     // current rate from setSampleRate
    };

    explicit Detector (double sampleRate);

    void   setSampleRate (double sampleRate);
    double getSampleRate() const;

    // Returns the latest spectrum from the tap. Returns a `Spectrum` with
    // `magnitudes == nullptr` if no FFT block has been produced yet. The
    // pointer is valid until the next call to `processLatestBlock`.
    Spectrum processLatestBlock (LockFreeRingBuffer<float>& tap);

private:
    double                             sampleRate_;
    juce::dsp::FFT                     fft_;
    juce::dsp::WindowingFunction<float> window_;
    std::vector<float>                 timeBuffer_;   // size kFftSize, in-place windowed
    std::vector<float>                 magnitudes_;   // size kNumBins
};
```

## Hot path (`processLatestBlock`)

1. `tap.read(timeBuffer_.data(), kFftSize)` → `readCount`. Partial reads are
   NORMAL — store the count, do NOT zero-fill above `readCount`.
2. Zero `timeBuffer_[readCount..kFftSize-1]` (so the Hann window at those
   positions contributes zero to the spectrum, not garbage). This zero-fill is
   of pre-allocated memory and is just an arithmetic loop — still no heap use.
3. `window_.multiplyWithWindowingTable (timeBuffer_.data(), kFftSize);`
4. `fft_.performFrequencyOnlyForwardTransform (timeBuffer_.data(), true);`
   After this, `timeBuffer_[0..kNumBins-1]` holds 513 magnitudes
   (DC, f1, f2, ..., fNyquist).
5. `std::memcpy (magnitudes_.data(), timeBuffer_.data(), kNumBins * sizeof(float));`
6. Return `{magnitudes_.data(), readCount, sampleRate_}`.

No allocation. All buffers sized in the constructor. The detector never calls
`tap.write()`; it is the sole reader.

## Tests (test_detector.cpp)

1. `Detector.MagnitudeOfSingleTone` — fill a `LockFreeRingBuffer<float>` with
   a 1 kHz sine at 48 kHz (full-scale amplitude, ≥ 4*kFftSize samples so the
   detector definitely gets a full block). Call `processLatestBlock` repeatedly
   until it returns `readCount == kFftSize`. Assert the bin closest to 1000 Hz
   (bin index = round(1000 / (48000/1024)) = round(21.33) = 21) dominates:
   `magnitudes[21] >= 5 * mean(magnitudes over all other bins)`. Document the
   expected bin in the test comment.

2. `Detector.PartialReadIsReportedAsPartial` — fill the tap with only
   kFftSize/2 samples, call `processLatestBlock` once, assert
   `spectrum.magnitudes != nullptr` AND `spectrum.readCount < kFftSize`.

3. `Detector.SetSampleRateUpdates` — construct at 48 kHz, call
   `setSampleRate (96000.0)`, assert `getSampleRate() == 96000.0`.

4. `Detector.HannWindowApplied` — fill the tap with `kFftSize` samples of
   constant 1.0 (DC). Call `processLatestBlock` once. Assert
   `magnitudes[0] < 0.5 * (magnitudes[1] + magnitudes[2] + magnitudes[3])`.
   Hann DC gain ≈ 0.5, so a rectangular window would put nearly all energy in
   bin 0; with Hann the energy spreads. (If you need a looser bound, justify
   it in the test comment; do not silently relax to make it pass.)

`test_detector.cpp` synthesizes inputs into a local `std::vector<float>` and a
local `LockFreeRingBuffer<float>`. Do NOT touch `AudioEngine`.

## Tests CMakeLists.txt

Append `test_detector.cpp` to the existing `add_executable(HandsFreeTests ...)`.
Do not create a second target. Do not alter GoogleTest setup.

## Commits

One commit: `feat: add detector thread FFT pipeline with tests`.

## Success criteria

1. `Detector.{h,cpp}` exist with the exact interface above.
2. `processLatestBlock` is allocation-free in steady state and uses the
   JUCE 9 API names actually present in the vendored headers.
3. Partial reads surface as `Spectrum::readCount < kFftSize`, not as a
   zero-padded full block.
4. 4 new tests pass; 25/25 total; `HandsFree` app target still builds.
5. Commit message is exactly the brief's.

## Interfaces produced

- `Detector::Spectrum` (carries `magnitudes`, `readCount`, `sampleRate`):
  Task 11 (peakiness) and Task 12 (harmonic-aware) consume this directly.
- `Detector::setSampleRate(double)`: Task 14 will wire this to
  `AudioEngine::audioDeviceAboutToStart`.

## Report contract

- **Model used:** exact model string
- **Tier escalation:** None, or what triggered it
- Status: DONE | DONE_WITH_CONCERNS | NEEDS_CONTEXT | BLOCKED
- Files with line deltas
- Full ctest output (25 tests)
- Commit hash
- Self-review:
  - Confirm hot path is allocation-free after construction
  - Confirm partial reads surface as partial
  - Confirm Hann is genuinely applied (test 4 must pass for the right reason)
  - Confirm you grepped the JUCE 9 headers and the API names match
- Concerns

NO SUBAGENTS. NO REVIEWER.
