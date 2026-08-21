# Task 8 Report — AudioEngine Class (ASIO I/O)

- **Model used:** `opencode/deepseek-v4-flash-free`
- **Tier escalation:** None
- **Status:** DONE

## Deliverables

| File | Lines | Role |
|---|---|---|
| `src/app/AudioEngine.h` | 90 | Class declaration (exact public interface from brief, `#pragma once`, `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`) |
| `src/app/AudioEngine.cpp` | 208 | Implementation (device lifecycle, real-time callback, mode control, latency reporting) |
| `CMakeLists.txt` | +2 | Adds `src/app/AudioEngine.cpp` and `.h` to `target_sources(HandsFree PRIVATE ...)` |

Commit: `14bb32c825373c7eb1da6e0377772ef3475a01c6` — `feat: add AudioEngine class with JUCE AudioIODeviceCallback integration` (3 files, 300 insertions)

## Build Verification

Toolchain: **MSVC** via Visual Studio 18 2026 Build Tools (`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`, MSVC 14.51.36231) + CMake 4.3.2, `-A x64`, `Release`.

```
cmake -S . -B build-task8-msvc -G "Visual Studio 18 2026" -A x64          -> Configuring done (54.4s), success
cmake --build build-task8-msvc --config Release --target HandsFree        -> exit 0
  HandsFree.vcxproj -> build-task8-msvc\HandsFree_artefacts\Release\AZ Soundtech Hands-free.exe
```

- `AudioEngine.cpp` recompiled in isolation (deleted its `.obj`, rebuilt): **0 warnings, exit 0**.
- `HandsFreeTests` builds and **18/18 tests pass** (ctest, no regression to Biquad/NotchChain/ring buffer).

**Toolchain notes:**
- MinGW is NOT usable for this project at all: vendored JUCE 9.0.1 has a hard `#error "MinGW is not supported"` in `juce_core/system/juce_TargetPlatform.h` (confirmed by direct compile). This is why the previous "MinGW vs MSVC" ruling exists.
- MSVC **is** installed (VS 18 Build Tools, found via vswhere — the earlier report's claim that MSVC was unavailable predates/disregards this install; the full GUI app now builds).
- **Pre-existing quirk (not mine):** JUCE 9's `juce_generate_juce_header` custom command (`juceaide header <Defs.txt> <JuceHeader.h>`) is never scheduled by MSBuild, so a fresh build fails with `C1083: Cannot open include file: 'JuceHeader.h'` for the app sources. Reproduced with the header deleted: MSBuild does not regenerate it. This is the exact error documented in the tasks-5-7 report (Concern 1). Workaround (proven): run once
  `juceaide.exe header "<build>\HandsFree_artefacts\JuceLibraryCode\Release\Defs.txt" "<build>\HandsFree_artefacts\JuceLibraryCode\JuceHeader.h"`
  then the build completes. `juceaide.exe` is built automatically at `external\JUCE\tools\extras\Build\juceaide\juceaide_artefacts\Custom\juceaide.exe`. Flagging for the orchestrator: a CI/MSVC build step may need this one-time generation (or a JUCE version bump).

## Deviations from the Brief (all required to compile against vendored JUCE 9.0.1)

1. **Callback signature (REQUIRED):** JUCE 9 replaced the legacy 5-arg `audioDeviceIOCallback(const float**, int, float**, int, int)` with the pure-virtual 6-arg `audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int, const AudioIODeviceCallbackContext&)`. The brief's signature does not exist in JUCE 9 (compile-verified). Adapted; the processing body is identical to the brief's description. `audioDeviceAboutToStart`/`audioDeviceStopped` are now pure virtual in JUCE 9 — both implemented.
2. **Private state wrapped in `std::atomic`:** `currentMode_`, `currentSampleRate_`, `currentBufferSize_`, `isRunning_` are `std::atomic` instead of plain members. Required to satisfy "NO locking in audioDeviceIOCallback" while the UI thread calls `setMode()`/`stop()`/queries concurrently. Public interface unchanged.
3. **Two extra private members:** `desiredDeviceType_` (default `"ASIO"` — honors the brief's "Open default ASIO device"; JUCE silently falls back if ASIO SDK is absent) and `desiredDeviceName_`. Both applied on the next `start()`.
4. **NotchChain construction:** `NotchChain` has no default ctor, so `std::array<NotchChain, 2>` is aggregate-initialized with `NotchChain(48000.0)` in the ctor init list.
5. **Latency API:** uses `AudioIODevice::getOutputLatencyInSamples()` (JUCE 9 renamed `getCurrentLatencySamples`). Returns seconds.

## Code Review Summary (self-review, no subagents)

Real-time safety (`audioDeviceIOCallbackWithContext`):
- **Zero allocation:** atomic load, `FloatVectorOperations::copy/clear` (memcpy/memset), `NotchChain::processSample` (pre-allocated `std::array<Biquad,16>`, verified no `new`/vector/map in the chain hot path — Task 7 audit). No `juce::String`, no logging in the callback.
- **Zero locks:** only `std::atomic` loads. `std::atomic<Mode/bool/int/double>` is lock-free on x64; no mutex anywhere on the audio path.
- **JUCE output contract:** every output channel is written or cleared before return (required — output buffers are not pre-cleared).
- Null-channel guards for both input and output arrays; `ch >= 2` channels (beyond the stereo DSP pair) pass through.
- Bypass mode skips the DSP entirely (input → output copy per the brief).

Lifecycle / threading:
- `stop()` uses `isRunning_.exchange(false)` for once-only teardown; `removeAudioCallback()` blocks until the audio thread releases the callback — only ever called from UI thread (documented in code).
- `start()` order: switch device type (synchronous in JUCE 9) → `initialiseWithDefaultDevices` (or `initialise` with preferred device name) → `addAudioCallback` (which fires `audioDeviceAboutToStart` if the device is already open) → `isRunning_ = true`.
- `audioDeviceAboutToStart`: captures sample rate + buffer size (atomics), resets both notch chains.
- `audioDeviceError` (may fire from any thread per JUCE docs): atomic store + `Logger` — error path only, not the hot path.
- `~AudioEngine()` calls `stop()`.
- API calls verified against the vendored JUCE 9.0.1 headers (`juce_AudioDeviceManager.h`, `juce_AudioIODevice.h`) — `initialise(int,int,const XmlElement*,bool,const String&,const AudioDeviceSetup*)`, `initialiseWithDefaultDevices`, `setCurrentAudioDeviceType`, `getCurrentDeviceTypeObject`, `getName`, `getCurrentSampleRate`, `getCurrentBufferSizeSamples`, `getOutputLatencyInSamples`.

## Self-Review Findings

- One real bug caught and fixed during build verification: the override signature mismatch (brief was written against pre-JUCE-9 API). Caught by the compiler as "marked override, but does not override" — resolved with the JUCE 9 signature.
- Grep audit of the callback body: no `new`, `malloc`, `std::vector`, `std::list`, `std::map`, `lock`, `mutex`, `wait`, `sleep` inside `audioDeviceIOCallbackWithContext`.
- No warnings with MSVC (`/W4`-class JUCE recommended flags) for the new file.

## Concerns

1. **NotchChain sample-rate lock-in (forward-looking, Task 9+):** chains are constructed at the nominal 48 kHz and `NotchChain` has no `setSampleRate()` API. When a later task starts calling `setNotch()` on a device running at 44.1 kHz (etc.), coefficients would be computed for 48 kHz. Task 8 sets no notches (brief: "reset + capture" only), so this is out of scope here, but a `NotchChain::setSampleRate(double)` (Task 7 file) or chain reconstruction in `audioDeviceAboutToStart` should land before notch-setting lands. `currentSampleRate_` is already captured and exposed for that fix.
2. **`JuceHeader.h` generation quirk (pre-existing):** see Build Verification. Not introduced by this task, but worth a CI workaround (one-time `juceaide header` invocation) so the MSVC CI build (`.github/workflows/build.yml`) passes.
3. **`depthDB` hint only:** unchanged from Task 7 (`NotchChain` records it but the biquad does not apply it) — no action needed for Task 8.
4. **Manual test still pending:** real audio I/O needs hardware (ASIO device); per the brief's test strategy, runtime verification (bypass passthrough, notch attenuation) is deferred to manual testing with an interface.