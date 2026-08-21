# Task 9: Sample-rate retarget + audio tap to ring buffer

## Context

Task 8 landed AudioEngine (commit 14bb32c) with a verified real-time-safe callback. Two carry-over
items from the Task 8 review are IN SCOPE for this task:

1. `NotchChain` is constructed at a hardcoded 48000.0 and has no way to retarget sample rate.
   Any `setNotch()` on a 44.1/88.2/96 kHz device would compute wrong coefficients. Must be fixed
   BEFORE any notch is ever set (Task 13 sets notches; the tap lands here).
2. `AudioEngine` currently has no tap: the detector (Task 10) has nothing to read from.

## Files

- Modify: `src/dsp/NotchChain.h` — add `setSampleRate(double)`
- Modify: `src/dsp/NotchChain.cpp` — implement retarget
- Modify: `src/app/AudioEngine.h` — add tap ring buffer + accessor
- Modify: `src/app/AudioEngine.cpp` — write tap in callback, set rate in audioDeviceAboutToStart
- Modify: `tests/test_notchchain.cpp` — add sample-rate tests
- Modify: `tests/CMakeLists.txt` only if a new test file is added (prefer extending the existing one)

## Part A: NotchChain::setSampleRate

Add to the public interface, after `reset()`:

```cpp
    void setSampleRate(double sampleRate);
    double getSampleRate() const;
```

Semantics (exact):
- Store the new sample rate.
- For every notch whose state is `Active`, recompute its biquad coefficients from the STORED
  `NotchInfo` (frequency, Q) against the new rate, so an active notch keeps its intended
  frequency in Hz across a rate change.
- Call `reset()` on filter state (old state is meaningless at a new rate).
- `Idle` notches need no coefficient work; their stored `NotchInfo` is retained.
- If the new rate equals the current rate, still safe to call (idempotent).
- Guard: ignore a non-positive sample rate (leave state unchanged).

Threading: `setSampleRate` is called from `audioDeviceAboutToStart` (UI/device thread, BEFORE the
callback is inserted into JUCE's dispatch list — verified in the Task 8 review), so it is NOT
required to be real-time safe. It must not allocate anyway, since it only rewrites coefficients
in the pre-allocated `std::array`.

## Part B: Audio tap in AudioEngine

Add to AudioEngine private members:

```cpp
    static constexpr size_t kTapCapacity = 8192;   // ~170 ms @ 48 kHz, power of 2
    LockFreeRingBuffer<float> tapBuffer_ { kTapCapacity };
```

Add to the public interface:

```cpp
    LockFreeRingBuffer<float>& getTapBuffer();
```

Tap semantics (exact):
- Tap the LEFT channel POST-notch (the signal actually leaving the app), so the detector sees the
  effect of notches already applied — this is what makes auto-release possible in Task 14.
- Tap in ALL modes including Bypass (the detector must still see the signal when bypassed).
- Write with the existing `write(const float*, size_t)` API. The detector may be slow or absent:
  a short write (or 0) is EXPECTED and must be silently tolerated — never block, never spin,
  never log in the callback.
- If there is no input channel 0 (null pointer or numInputChannels == 0), write nothing.

Real-time constraints (unchanged from Task 8, re-verify):
- No allocation, no locks, no logging, no juce::String in the callback.
- The tap must not change the audio written to the output.

Implementation note: the callback currently processes samples for output. Accumulate the
post-notch left-channel samples into a small stack buffer (`float tap[...]` sized by a
compile-time max block, or write straight from the output buffer after processing when the
output pointer for channel 0 is non-null) and issue ONE `write()` per callback rather than one
per sample. A per-sample write is correct but wasteful; a single bulk write is preferred.
Do NOT introduce a heap buffer for this.

## Part C: Wire sample rate

In `audioDeviceAboutToStart`, after capturing `currentSampleRate_`, call `setSampleRate` on BOTH
notch chains before the audio thread can run. Keep the existing chain `reset()` behavior.

## Tests (extend tests/test_notchchain.cpp)

Add these, following the existing test style in that file:

```cpp
TEST(NotchChain, SetSampleRateKeepsActiveNotchFrequency)
TEST(NotchChain, SetSampleRateIgnoresNonPositive)
TEST(NotchChain, GetSampleRateReflectsConstructorAndSetter)
```

For `SetSampleRateKeepsActiveNotchFrequency`, the meaningful assertion is behavioral, not
coefficient-equality: set a notch at 1000 Hz at 48 kHz, call `setSampleRate(96000.0)`, then drive
a 1000 Hz sine AT 96 kHz through the chain and assert the output is still attenuated below the
same threshold the existing biquad/notch tests use (< 0.25 after the transient). Also assert a
control tone one octave away is still passed (gain ratio > 0.9), so the test cannot pass by the
filter simply killing everything.

Do not weaken the existing 18 tests to make room.

## Verification

Run, and paste the output in the report:

```
cmake --build <your msvc build dir> --config Release
ctest -C Release --output-on-failure
```

Expected: 21/21 tests pass (18 existing + 3 new). The `HandsFree` app target must still build.

Note on the known JUCE header quirk (documented in tasks-5-7 and Task 8): if MSBuild does not
schedule JUCE's header generation, the one-time `juceaide header <Defs.txt> <JuceHeader.h>`
workaround applies. Use it if needed and say so; it is not a defect introduced by this task.

## Commits

Two commits, in this order:

1. `feat: add sample-rate retarget to NotchChain with tests`
2. `feat: tap post-notch left channel into lock-free ring buffer`

## Success criteria

1. `NotchChain::setSampleRate` recomputes active-notch coefficients and resets filter state.
2. `AudioEngine` owns a `LockFreeRingBuffer<float>` tap, written once per callback, post-notch,
   left channel, in every mode, with short writes tolerated.
3. `audioDeviceAboutToStart` propagates the device rate into both chains before audio runs.
4. 21/21 tests pass; app target still builds.
5. Callback remains allocation-free and lock-free.

## Interfaces produced

- `NotchChain::setSampleRate(double)` / `getSampleRate()` — consumed by AudioEngine and Task 13.
- `AudioEngine::getTapBuffer()` → `LockFreeRingBuffer<float>&` — the read side is the detector
  thread in Task 10. Task 10 will read 1024-sample blocks with 512 hop from this buffer.

## Report contract

- **Model used:** exact model string
- **Tier escalation:** None, or what triggered it
- Status: DONE | DONE_WITH_CONCERNS | NEEDS_CONTEXT | BLOCKED
- Files modified with line deltas
- Full ctest output (21 tests)
- Both commit hashes + messages
- Self-review: confirm no allocation/locks in callback; explain the tap write strategy chosen
- Concerns
