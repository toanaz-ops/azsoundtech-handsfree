# Task 9 Report — Sample-rate retarget + audio tap to ring buffer

- **Model used:** `opencode/deepseek-v4-flash-free` (Tier 1 default)
- **Tier escalation:** None
- **Status:** DONE

## Deliverables

### Part A — `NotchChain::setSampleRate(double)` / `getSampleRate()`

Added to the public interface after `reset()`, exactly as specified. Semantics:

- Stores the new rate, then recomputes coefficients for every notch whose
  `state == Active` from its STORED `NotchInfo` (frequency, Q) via
  `Biquad::setNotchFilter(freq, Q, newRate)` — an active notch keeps its
  intended frequency in Hz across a rate change.
- Calls `reset()` on the whole chain (old filter state is meaningless at a new
  rate; this also clears stale state in Idle slots that were previously
  active — `setNotchFilter` alone would not touch them).
- Idle notches: no coefficient work, stored `NotchInfo` retained.
- Idempotent when the rate is unchanged.
- Guard: `sampleRate <= 0.0` returns immediately, leaving all state untouched.
- Allocation-free: only rewrites pre-allocated `std::array` slots. Not real-time
  critical (called from `audioDeviceAboutToStart` before the callback is
  dispatched), but never allocates anyway.

### Part B — Tap ring buffer in AudioEngine

- Private: `static constexpr size_t kTapCapacity = 8192;` (power of 2, ~170 ms
  @ 48 kHz) + `LockFreeRingBuffer<float> tapBuffer_ { kTapCapacity };`
- Public: `LockFreeRingBuffer<float>& getTapBuffer();`
- Written once per callback, POST-notch, LEFT channel, in EVERY mode including
  Bypass, straight from the already-processed output buffer. Short/zero writes
  silently tolerated (return value ignored with `(void)`); never blocks, never
  spins, never logs. If input channel 0 is absent (null array / 0 channels /
  null pointer), nothing is written.

### Part C — Rate propagation

`audioDeviceAboutToStart` now calls `setSampleRate` on BOTH chains with
`device->getCurrentSampleRate()` after capturing `currentSampleRate_` (JUCE
invokes this before inserting the callback into its dispatch list, so no
synchronization needed). The existing `reset()` loop is preserved. A
non-positive rate (impossible on a running device) is safely ignored by the
chain's guard.

## Files modified (line deltas)

| File | Delta | Commit |
|---|---|---|
| `src/dsp/NotchChain.h` | +9 | 1 |
| `src/dsp/NotchChain.cpp` | +33 | 1 |
| `tests/test_notchchain.cpp` | +66 (3 new tests) | 1 |
| `src/app/AudioEngine.h` | +19/−2 | 2 |
| `src/app/AudioEngine.cpp` | +37/−6 | 2 |

`tests/CMakeLists.txt` unchanged — the three tests extend the existing
`test_notchchain.cpp` per the brief.

## Commits

1. `7c1718a` — `feat: add sample-rate retarget to NotchChain with tests` (3 files, 108 insertions)
2. `0f7d480` — `feat: tap post-notch left channel into lock-free ring buffer` (2 files, 48 insertions, 8 deletions)

## Build verification (real output)

Toolchain: MSVC via Visual Studio 18 2026, `-A x64`, `Release`, dir `build-task8-msvc`
(same dir as Task 8). The known JUCE header quirk hit: MSBuild did not schedule
`juceaide header`, and `JuceHeader.h` was missing → `C1083` on `AudioEngine.cpp`.
Applied the documented one-time workaround:

```
juceaide.exe header build-task8-msvc\HandsFree_artefacts\JuceLibraryCode\Release\Defs.txt
                build-task8-msvc\HandsFree_artefacts\JuceLibraryCode\JuceHeader.h
```

(exit 0, header generated). Then the full build completed; the `HandsFree` app
target (`AZ Soundtech Hands-free.exe`) and `HandsFreeTests.exe` both built.

```
cmake --build build-task8-msvc --config Release
  ... AudioEngine.cpp (2 pre-existing C4324 padding warnings from LockFreeRingBuffer's alignas(64), informational)
  HandsFree.vcxproj -> build-task8-msvc\HandsFree_artefacts\Release\AZ Soundtech Hands-free.exe
  HandsFreeTests.vcxproj -> build-task8-msvc\tests\Release\HandsFreeTests.exe
  (exit 0)
```

### ctest (full output, 21/21)

```
Test project D:/DEV CAVE EP3/PROJECT005-AZ-handsfree/build-task8-msvc
      Start  1: LockFreeRingBuffer.WriteAndReadSingleSample
 1/21 Test  #1: LockFreeRingBuffer.WriteAndReadSingleSample ............   Passed    0.01 sec
      Start  2: LockFreeRingBuffer.AvailableSpace
 2/21 Test  #2: LockFreeRingBuffer.AvailableSpace ......................   Passed    0.01 sec
      Start  3: LockFreeRingBuffer.Wraparound
 3/21 Test  #3: LockFreeRingBuffer.Wraparound ..........................   Passed    0.01 sec
      Start  4: LockFreeRingBuffer.MultipleWritesAndReads
 4/21 Test  #4: LockFreeRingBuffer.MultipleWritesAndReads ..............   Passed    0.01 sec
      Start  5: LockFreeRingBuffer.ReadEmptyReturnsZero
 5/21 Test  #5: LockFreeRingBuffer.ReadEmptyReturnsZero ................   Passed    0.01 sec
      Start  6: LockFreeRingBuffer.WriteFullStopsAtCapacity
 6/21 Test  #6: LockFreeRingBuffer.WriteFullStopsAtCapacity ............   Passed    0.01 sec
      Start  7: Biquad.NotchAttenuatesTargetFrequency
 7/21 Test  #7: Biquad.NotchAttenuatesTargetFrequency ..................   Passed    0.01 sec
      Start  8: Biquad.NotchPassesOffTargetFrequency
 8/21 Test  #8: Biquad.NotchPassesOffTargetFrequency ...................   Passed    0.01 sec
      Start  9: Biquad.ResetClearsState
 9/21 Test  #9: Biquad.ResetClearsState ................................   Passed    0.01 sec
      Start 10: Biquad.HandlesDifferentSampleRates
10/21 Test #10: Biquad.HandlesDifferentSampleRates .....................   Passed    0.01 sec
      Start 11: Biquad.CoefficientValidity
11/21 Test #11: Biquad.CoefficientValidity .............................   Passed    0.01 sec
      Start 12: NotchChain.InitiallyPassthrough
12/21 Test #12: NotchChain.InitiallyPassthrough ........................   Passed    0.01 sec
      Start 13: NotchChain.SetNotchAttenuates
13/21 Test #13: NotchChain.SetNotchAttenuates ..........................   Passed    0.01 sec
      Start 14: NotchChain.ClearNotchRestoresPassthrough
14/21 Test #14: NotchChain.ClearNotchRestoresPassthrough ...............   Passed    0.01 sec
      Start 15: NotchChain.MultipleActiveNotches
15/21 Test #15: NotchChain.MultipleActiveNotches .......................   Passed    0.01 sec
      Start 16: NotchChain.MaxNotches16
16/21 Test #16: NotchChain.MaxNotches16 ................................   Passed    0.01 sec
      Start 17: NotchChain.ActiveNotchCount
17/21 Test #17: NotchChain.ActiveNotchCount ............................   Passed    0.01 sec
      Start 18: NotchChain.OutOfRangeIndexIgnored
18/21 Test #18: NotchChain.OutOfRangeIndexIgnored ......................   Passed    0.01 sec
      Start 19: NotchChain.SetSampleRateKeepsActiveNotchFrequency
19/21 Test #19: NotchChain.SetSampleRateKeepsActiveNotchFrequency ......   Passed    0.01 sec
      Start 20: NotchChain.SetSampleRateIgnoresNonPositive
20/21 Test #20: NotchChain.SetSampleRateIgnoresNonPositive .............   Passed    0.01 sec
      Start 21: NotchChain.GetSampleRateReflectsConstructorAndSetter
21/21 Test #21: NotchChain.GetSampleRateReflectsConstructorAndSetter ...   Passed    0.01 sec

100% tests passed, 0 tests failed out of 21

Total Test time (real) =   0.19 sec
```

The 3 new tests: `SetSampleRateKeepsActiveNotchFrequency` asserts behavior per
the ruling — a 1000 Hz notch set at 48 kHz, then `setSampleRate(96000.0)`, still
attenuates a 1000 Hz sine driven at 96 kHz below `< 0.25` RMS (post-transient)
while a 2000 Hz control tone passes with gain ratio `> 0.9` — not
coefficient-equality. `SetSampleRateIgnoresNonPositive` (0.0 and −96000.0 are
no-ops, behavior unchanged). `GetSampleRateReflectsConstructorAndSetter`
(44100 → 96000, idempotent re-set).

## Self-review — callback remains allocation-free and lock-free

`audioDeviceIOCallbackWithContext` body (unchanged parts + the new tap):

- **No allocation:** the only addition is `tapBuffer_.write()` — a pre-allocated
  `std::vector<float>` ring buffer inside the engine, `std::copy` into existing
  slots, atomic index stores. No `new`, no `std::vector` construction, no
  `juce::String`, no logging on the hot path (grep-verified).
- **No locks:** only `std::atomic` loads/stores (mode load at the top;
  `writePos_`/`readPos_` in the ring buffer with acquire/release). No mutex,
  no spin-wait, no blocking anywhere in the callback.
- **Tap does not change the output audio:** it reads `outputChannelData[0]`
  AFTER all processing; it never writes into the output. Bypass DSP behavior
  is untouched (the copy/clear path is unchanged).
- **JUCE output contract preserved:** every output channel is still written or
  cleared within the channel loop before the tap runs.

**Tap write strategy chosen:** one bulk `write()` per callback, straight from
the already-processed output buffer (`outputChannelData[0]`, which is
post-notch in DSP mode and the bypass copy in Bypass mode — in both cases the
signal actually leaving the app). No stack scratch buffer and no heap buffer;
zero extra copies. The write is executed after the channel loop so channel 0 is
guaranteed final regardless of mode/missing-input path. Return value ignored
(`(void)`): short writes or 0 are expected when the detector is slow/absent and
are silently tolerated, per the ring buffer's SPSC contract (producer is the
audio thread; consumer will be the detector thread in Task 10, reading
1024-sample blocks with a 512-sample hop).

## Concerns

1. **Two informational C4324 warnings** (`structure was padded due to alignment
   specifier`) now surface in the app target when `AudioEngine.cpp` first
   instantiates `LockFreeRingBuffer<float>`. They originate from the
   pre-existing `alignas(64)` cache-padding in `LockFreeRingBuffer.h` (Tasks
   5–7) and are intended behavior; nothing was changed to cause them beyond
   first template instantiation in the app target. No code impact.
2. **`tests/test_notchchain.cpp` retains the pre-existing "no newline at end of
   file" style** (the original file also ended without one); no change made to
   preserve minimal diff. Harmless.
3. **JUCE header quirk re-encountered** (documented, not mine): a fresh MSVC
   build needs the one-time `juceaide header` invocation; a CI workaround is
   still worth adding (see Task 8 concern 2). `JuceHeader.h` is a generated
   artifact inside `build-task8-msvc/` — untracked, no repo pollution.
4. **`open code-harness-diagram.html`** untracked file at repo root pre-dates
   this session (present in the initial `git status`); unrelated to this task,
   left untouched.
5. **Real-device behavior still unverified** (unchanged from Task 8): rate
   propagation and tap correctness on actual ASIO hardware need manual testing;
   unit-level behavioral coverage is in place (rate retarget via NotchChain
   tests; tap path is compile-verified and lock-free by construction).
