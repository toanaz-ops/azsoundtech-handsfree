# Fix pass 1 — implementation report

**Model:** claude-opus-5
**Branch:** `fix/review-pass-1` (from `main` @ `55c0b12`)
**Status:** COMPLETE — both commits landed, both targets build, all tests pass.
**Not merged, not pushed.**

| | |
|---|---|
| Commit 1 (Part A) | `ff2063e` — `fix: guard notch coefficients and suppress denormals in the audio callback` |
| Commit 2 (Part B) | `3d1bc68` — `fix: define tap accessor, drain tap on restart, and count tap drops` |
| Tests before | 40 / 40 |
| Tests after | 55 / 55 |

## Build environment

The brief's build directory
(`.../2dded5f4-.../scratchpad/build-fixpass1`) is too long for MSVC's
FileTracker on this machine (FTK1011), so per the orchestrator's instruction I
used `C:/Users/id_az/AppData/Local/Temp/fp1`, configured with
`-G "Visual Studio 18 2026" -A x64`, `--config Release`. A second, fully clean
configure at `C:/Users/id_az/AppData/Local/Temp/fp2` was used for final
verification, per the `build-verification-2026-08-21` memory note (build-dir
evidence is evidence about a machine, not about the repo).

Final `ctest -C Release` on the clean `fp2` tree, at commit `3d1bc68`:

```
100% tests passed, 0 tests failed out of 55

Total Test time (real) =   1.86 sec
```

(40 tests at `55c0b12`, 49 after commit 1, 55 after commit 2.)

Both targets build. **Not zero warnings:** `HandsFree` emits two pre-existing
C4324 warnings ("structure was padded due to alignment specifier") from the
`alignas(64)` in `LockFreeRingBuffer.h`. They are present at `55c0b12` as well;
this pass neither introduced nor fixed them. `HandsFreeTests` builds clean.

---

## A1 — denormal limit cycle

**The finding reproduces. Fully.** Both halves of
`Biquad.SilenceDoesNotLeaveDenormalState` are green.

First I confirmed the premise the brief asked me to check rather than trust:
there is no `/fp:fast`, no `-ffast-math` and no FTZ/MXCSR flag anywhere in
`CMakeLists.txt`, `tests/CMakeLists.txt` or the JUCE CMake helpers.
`juce_recommended_config_flags` adds only `/Ox /MP /EHsc` on MSVC. The finding
therefore stands.

**Measured, 1 kHz / Q=10 notch at 48 kHz, charged with 4800 samples of
full-scale sine then fed 240,000 samples of exact `0.0` (5 s):**

| | tail sample | `fpclassify` |
|---|---|---|
| without `ScopedNoDenormals` | `1.9268560187808615e-322` | `FP_SUBNORMAL` |
| with `ScopedNoDenormals` | `0.0` exactly | `FP_ZERO` |

An independent double-precision reference model in Python (no numpy) predicted
`1.93e-322` for the same run, and put the first subnormal output at silence
sample **107,557**. The mechanism is exactly as the brief describes: the pole
radius is `0.99349485`, the smallest positive double is `4.94e-324`, and
`0.99349485 * 4.94e-324 = 4.907e-324` rounds *back* to `4.94e-324`. The state
never reaches zero.

**Measured cost**, 16 notches in series, Release, 5 s of silence, after warming
past the point where the state falls subnormal so the figure is steady state
and not the mixed ramp (3 runs):

| | ms per second of audio |
|---|---|
| denormals enabled | 77.2 / 75.3 / 76.8 |
| `ScopedNoDenormals` | 0.82 / 1.05 / 1.01 |

That is a **72x–94x** penalty, corroborating the reviewer's 94 ms/s figure
closely. My "with guard" figure (~0.9 ms/s) is lower than the reviewer's
1.7 ms/s baseline — different harness, so I would not read anything into the
difference. Without the warm-up, averaging over the whole 5 s (45% of which is
still normal arithmetic) the ratio measures only 13.7x, which is why I report
the steady-state number: it is the one that matters when the mic is muted for
more than ~2.3 s.

**Watching it fail.** The guarded half cannot fail against the shipped code,
because the fix lives in `AudioEngine.cpp` and the test lives at the `Biquad`
level. So I forced it: I temporarily deleted the `juce::ScopedNoDenormals`
declaration from the test's first block and rebuilt. Real failure:

```
test_biquad.cpp(231): error: Expected equality of these values:
  guarded
    Which is: 1.9268560187808615e-322
  0.0
    Which is: 0
tail sample under ScopedNoDenormals = 1.9268560187808615e-322
test_biquad.cpp(232): error: Expected: (std::fpclassify(guarded)) != ((-2)), actual: -2 vs -2
```

Then restored it. Note `EXPECT_EQ`, not `EXPECT_DOUBLE_EQ`: 4 ULP of `0.0` is
about `2e-323`, which would have accepted the very subnormal the test exists to
rule out.

**Fix:** `juce::ScopedNoDenormals noDenormals;` is now the first statement of
`AudioEngine::audioDeviceIOCallbackWithContext`.

## A2 — divergent coefficients

**Measured peak |output|, 30 kHz notch set legally at 96 kHz, chain retargeted
to 44.1 kHz, unit impulse followed by 1000 zeros:**

| | peak |
|---|---|
| before the fix | `4.1143098682246339e+18` |
| after the fix | `1.0` (notch deactivated, chain is a pass-through) |

The test asserts `< 2.0`; eighteen orders of magnitude of headroom.

**Fix, as specified:**

1. `Biquad::setNotchFilter` returns `bool` and rejects `sampleRate <= 0`,
   `Q <= 0`, `freq <= 0`, `freq >= sampleRate / 2`, leaving the filter's
   coefficients *and* state untouched on rejection. Branch-only: no
   allocation, no logging, no exceptions, still audio-thread-safe.
2. `NotchChain::setSampleRate` deactivates (does not clamp) any Active notch
   the biquad refuses at the new rate, retaining its stored `NotchInfo`.

**One addition beyond the brief's literal text, and why.** `NotchChain::setNotch`
now returns early when `setNotchFilter` rejects the parameters, instead of
marking the slot Active anyway. Without this, `setNotch(0, 30000, 10, -12)` at
48 kHz would leave the slot reported as an Active 30 kHz notch while
`filters_[0]` still held whatever design was there before — the chain would
advertise one frequency and filter another. Covered by
`NotchChain.SetNotchIgnoresParametersTheBiquadRejects`. I left
`NotchChain::setNotch` returning `void` rather than propagating the `bool`;
that is a Task 13 API decision, not a defect fix.

## A3 — spec amendment

`docs/superpowers/specs/2026-08-19-az-soundtech-hands-free-design.md` §8, the
bullet that read *"Sample rate thay đổi: clear notch, re-init."*, now states
that a rate change **retargets** active notches to preserve their frequency in
Hz, that any notch no longer below the new Nyquist is **deactivated** with its
stored parameters retained, that it is deliberately not clamped, and gives the
rationale (the acoustic environment does not change when the audio device
does). §8's other bullets are untouched.

`NotchChain.SetSampleRateKeepsActiveNotchFrequency` was **not** weakened or
deleted.

## B1 — `getTapBuffer()` was declared and never defined

**Reproduced verbatim.** Wiring `AudioEngine` into the test target and calling
the accessor produced exactly the predicted failure:

```
test_audioengine.obj : error LNK2019: unresolved external symbol
"public: class LockFreeRingBuffer<float> & __cdecl AudioEngine::getTapBuffer(void)"
... referenced in function ... AudioEngine_TapBufferAccessorIsDefinedAndUsable_Test::TestBody
fatal error LNK1120: 1 unresolved externals
```

**A unit test IS viable**, so I added four (`tests/test_audioengine.cpp`).
Constructing a `juce::AudioDeviceManager` opens no device — `start()` does, and
no test calls it. `juce::ScopedJuceInitialiser_GUI` provides the MessageManager
that JUCE's broadcaster and leak-detector machinery expect.
`audioDeviceIOCallbackWithContext` and `audioDeviceAboutToStart` are public
(they are `juce::AudioIODeviceCallback` overrides), so the tests drive the
callback directly with fabricated buffers — which is what makes B2 and B3
testable for real rather than by inspection.

**This required one change to production code that the brief did not ask for,
and it is the change I would most like reviewed.** `AudioEngine.h` included
`<JuceHeader.h>`, which only exists for targets created by a `juce_add_*`
function; a plain `add_executable` test target can never include it
(`juce_generate_juce_header` hard-errors on such a target). I replaced it with
`#include <juce_audio_devices/juce_audio_devices.h>`, which is what the file
actually needs and is the modern JUCE-CMake idiom. This is arguably the root
cause of the defect itself: `AudioEngine` was untestable, so nothing ever
linked `getTapBuffer()`. It is also the same `JuceHeader.h` coupling that
produced the false "build verified" claim recorded in
`memory/build-verification-2026-08-21.md`. `HandsFree` still builds; `main.cpp`
and `MainComponent.h` include `<JuceHeader.h>` themselves and are unaffected.

**Consumer-only view: considered, not built.** `getTapBuffer()` is defined as
declared, returning `LockFreeRingBuffer<float>&`, with the SPSC caller contract
documented at the declaration. `Detector::processLatestBlock` takes
`LockFreeRingBuffer<float>&` directly, so a view type would mean changing the
detector's signature too — more churn than the current single-caller risk
justifies. The comment says so, and says to add the view when a second caller
appears.

## B2 — stale audio across a rate change

- `LockFreeRingBuffer::clear()` advances `readPos_` to `writePos_` (rather than
  zeroing both, so the monotonic indices keep increasing). The
  "neither thread may be running" precondition is documented at the
  declaration, along with why `audioDeviceAboutToStart()` satisfies it.
- `Detector::reset()` zeroes `history_`. Allocation-free.
- `AudioEngine::audioDeviceAboutToStart()` calls `tapBuffer_.clear()`,
  unconditionally — not inside the `device != nullptr` branch, since the drain
  has nothing to do with whether a device pointer was supplied.
- Wiring `Detector::reset()` into the device path is left to Task 14, as the
  brief directs.

Tests: `LockFreeRingBuffer.ClearDiscardsPendingData`,
`Detector.ResetClearsAnalysisWindow` (bit-exact against a freshly constructed
detector fed the same single hop, with a sanity assertion that the primed and
fresh spectra really do differ, or the comparison would prove nothing), and
`AudioEngine.DeviceRestartDrainsTheTap`.

## B3 — tap drops

`std::atomic<std::uint64_t> tapDropCount_`, `fetch_add(..., relaxed)` when
`written < requested`, exposed via `getTapDropCount()`. Tested at both the
total-drop boundary (ring holds 8192; eight 1024-sample callbacks fit exactly,
the ninth drops all 1024) and the partial case that actually splices (drain
100, next 1024-sample block writes 100 and drops 924).

## B4 — `Detector::sampleRate_`

Now `std::atomic<double>`, relaxed on every access. Added a
`static_assert(std::atomic<double>::is_always_lock_free)` in `Detector.cpp` —
an atomic that silently fell back to a mutex would put a lock on the detector's
hot path, which is the opposite of the point. There is no runtime test for a
data race that is not yet reachable; the assert is the honest form of
verification here.

## B5 — the short-hop test

**Route chosen: a narrow test accessor.** `Detector::getAnalysisWindowForTest()`
returns `const float*` to `history_`. I chose it over the "assert on the
spectrum" alternative because that alternative does not work: the natural probe
is a sinusoid, and a *shifted* sinusoid has an identical magnitude spectrum, so
a position error is invisible to it. Any signal that would expose the shift
spectrally would be a contrived construction whose expected magnitudes need
their own derivation — a bigger and more fragile surface than one const
pointer. The name carries the restriction, and the header says outright not to
build product behaviour on it.

`Detector.PartialReadIsReportedAsPartial` now feeds a ramp (`value == index+1`,
so a sample identifies its own position and can never be confused with the zero
prefix), runs one full hop then one 200-sample hop, and asserts the window is
312 zeros followed by `1.0 .. 712.0`, contiguous and right-aligned. Integers up
to 2^24 are exact in `float`, so these are exact equalities. The original
`readCount` assertions are kept.

**The mutation, actually run.** Applied the reviewer's exact change to
`Detector.cpp:54` (`+ readCount` → `+ kHopSize`), rebuilt, ran the full suite:

```
33/55 Test #33: Detector.PartialReadIsReportedAsPartial ...***Failed
98% tests passed, 1 tests failed out of 55
```

```
test_detector.cpp(131): error: Expected equality of these values:
  window[i]
    Which is: 1
  0.0f
    Which is: 0
stale sample at history_[0]
```

Exactly one test caught it — the new one. Reverted the mutation, rebuilt:
**100% tests passed, 0 tests failed out of 55.**

---

## Things in the brief I concluded were wrong

**1. The "3.7e77" figure in A2 is wrong for the run the brief prescribes.**
The brief says to "feed a unit impulse followed by 1000 zeros" and that
"without the fix this reaches ~3.7e77". It does not. Over exactly that run the
measured peak is `4.11e18`, and an independent Python reference agrees to four
significant figures. The pole radius is 1.046351, so `1.046351^1000 ≈ 4.7e19`;
reaching 3.7e77 needs roughly 3,900 samples, not 1,000. The reviewer's *other*
two numbers for this case reproduce exactly — `|y| > 1e3` first at sample
**208**, i.e. 4.7 ms at 44.1 kHz. Nothing about the fix changes; the `< 2.0`
bound is sound either way. But the report contract asked for the measured
number and 4.11e18 is it.

**2. B5's description of where the mutation's damage lands is wrong.**
The brief says the mutation leaves "312 stale samples at `history_[712..1023]`".
It does not. With `readCount = 200`, `retained = 1024 - 200 = 824`, so the
mutated `memmove` copies 824 floats from `history_.data() + 512`, i.e. it reads
`history_[512..1335]` — **312 elements past the end of a 1024-element
`std::vector`**. The mutation is therefore also an out-of-bounds read, and the
corruption it leaves is at `history_[0..511]` (which receive the old
`[512..1023]`), not at `[712..1023]`. My test fails on `history_[0]`, the very
first element. The mutation is real and the concern is real; only the
description of the damage was off.

**3. A1's absolute CPU figures did not reproduce, though the effect did.**
The reviewer's 1.7 ms/s → 94 ms/s becomes ~0.9 ms/s → ~77 ms/s in my harness.
Same phenomenon, same order of magnitude, different rig. I report mine because
it is the one I ran.

**4. The prescribed build directory is unusable on this machine** (MSVC
FileTracker FTK1011 on the long scratchpad path). Noted above.

## Existing tests

**No existing test was modified, weakened or deleted.** All 40 still pass.

One existing test changed *meaning* without changing text, and it is worth
recording: `Biquad.CoefficientValidity` calls
`setNotchFilter(0.0, 10.0, 48000.0)` and asserts the output is finite. That
call used to compute `alpha = 0` (poles on the unit circle) and happened to
produce a finite first sample; it is now **rejected**, and the assertion passes
because an untouched pass-through is finite. The test still asserts something
true and useful, but it is no longer asserting what its author thought.
`RejectsNonPositiveFrequencyAndSampleRate` now pins the intended behaviour
explicitly.

## Residual concerns

- **Nothing bounds the detector's output frequency range yet.** A2's guard
  stops a bad notch from destroying the signal path, but the scenario that
  produced it — the detector parking a notch at 30 kHz because nothing limits
  its bin range — is untouched. That belongs to Tasks 11–14, and it is now a
  silent deactivation rather than a loud explosion, which is safer but also
  quieter. Whatever surfaces `getTapDropCount()` in the GUI should surface
  "notch deactivated by rate change" too.
- **`getTapBuffer()` still hands out write access.** See B1.
- **`Detector::reset()` is provided but not called** from any device path, by
  the brief's instruction. Until Task 14 wires it, B2 is fixed on the producer
  side only: the ring is drained, but a detector that had already pulled old
  samples into `history_` still holds them.
- **The `AudioEngine.h` include change is the one item here I would flag for a
  second opinion.** It is behaviour-neutral and it is what made B1 testable,
  but it is production-code surgery the brief did not request.
