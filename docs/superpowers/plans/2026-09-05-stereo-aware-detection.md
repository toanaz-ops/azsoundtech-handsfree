# Stereo-aware Detection (Lane S) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each lane of a stereo slot gets its own spectrum, score and notch placement, with a per-slot LINK toggle that restores today's "one side confirms, both sides get cut" behaviour.

**Architecture:** `AudioEngine` taps both lanes of every slot into two SPSC rings per slot. `NotchController` keeps one detector thread per slot but owns two `LaneAnalysis` bundles (Detector + PeakinessAnalyzer + CandidateScorer + persistence) and drains both rings in lockstep. Placement policy is a function of `width_`, whether a lane-1 tap exists, and an atomic `linked_` flag. `NotchCommand.channel`, `model_[channel][index]` and `notchChains_[slot][lane]` already address lanes; nothing on the audio thread changes except writing a second tap.

**Tech Stack:** C++17, JUCE 8 (juce_dsp, juce_gui_basics), GoogleTest via ctest, CMake + Visual Studio 18 2026 generator, MSVC.

**Spec:** `docs/superpowers/specs/2026-09-05-stereo-aware-detection-design.md` (read it first; decisions S-1..S-10 are binding).

## Global Constraints

- Audio thread: zero allocation, zero locks, zero logging. A second `write()` per stereo slot is the only new audio-thread work.
- SPSC: the audio callback is the sole producer of every tap ring; the slot's detector thread is the sole consumer of both of that slot's rings.
- Default placement mode is INDEP (`linked_ = false`). The legacy 4-argument `NotchController` ctor (no lane-1 tap) behaves LINKED regardless of the flag (S-6).
- LINKED placement picks the first index free on **every** lane `< width_` (S-7).
- `adoptPreset` always uses the file's `index` (S-8).
- Preset version stays `"1.0"`; new keys `lane` (notch, optional, 0|1) and `linked` (slot, optional bool). Invalid `lane` rejects the whole file in the SHAPE pass (S-9).
- No preset SAVE path is added (S-10).
- Every new test states in a comment which production change turns it red (repo convention).
- Build commands (from the worktree root):
  ```
  cmake --build build --config Release
  cd build && ctest -C Release --output-on-failure -R <Filter>
  ```
  Full-suite gate before every commit that touches `src/`: `ctest -C Release` must report `100% tests passed`.
- Commit with explicit paths. Never `git add -A`.
- Expected level change is documented in spec §3; do not alter depth/Q/threshold defaults.

## File Structure

| File | Responsibility after this plan |
|---|---|
| `src/app/AudioEngine.h/.cpp` | Two tap rings per slot, `getTapBuffer(slot, lane)`, `getTapDropCount(slot, lane)`; callback records `tapSource[slot][lane]` |
| `src/app/NotchController.h/.cpp` | `LaneAnalysis` ×2, lockstep drain, per-lane detection, `setLinked`, `setLaneAsymmetryBonus`, lane-aware free-index search, `setWidth` clearing lane-1, snapshot with `magnitudes[lane]`/`laneCount`/`linked` |
| `src/app/PresetManager.h/.cpp` | `PresetNotch::lane`, `PresetSlot::linked`, read + write |
| `src/app/MainComponent.h/.cpp` | Controllers built with both taps, `slotLinked_[]`, `onSlotLinkChanged` wiring, `loadPreset` applies `linked` |
| `src/gui/SlotPanel.h/.cpp` | Per-row LINK/INDEP segmented control, `onSlotLinkChanged`, `slotLinkedProvider` |
| `src/gui/NotchListPanel.h/.cpp` | LANE column (`L`/`R`) |
| `src/gui/SpectrumView.h/.cpp` | `L`/`R` display-lane selector, dashed stem + `R` tag for lane-1 markers |
| `docs/KY-THUAT-CHONG-HU.md`, `docs/GIOI-THIEU.md` | Updated in the same commit as the behaviour |
| `tests/test_audioengine.cpp`, `tests/test_notchcontroller.cpp`, `tests/test_presetmanager.cpp`, `tests/test_slotpanel.cpp`, `tests/test_notchlistpanel.cpp`, `tests/test_spectrumview.cpp` | New tests per spec §5 |

---

### Task 1: Two tap rings per slot in `AudioEngine`

**Files:**
- Modify: `src/app/AudioEngine.h:155-195` (accessor comments + declarations), `src/app/AudioEngine.h:268-280` (`tapBuffers_`, `tapDropCounts_`)
- Modify: `src/app/AudioEngine.cpp:288-309` (accessors), `src/app/AudioEngine.cpp:500-548` (lane table), `src/app/AudioEngine.cpp:634-665` (tap write)
- Test: `tests/test_audioengine.cpp`

**Interfaces:**
- Produces:
  ```cpp
  LockFreeRingBuffer<float>& getTapBuffer (int slot, int lane);   // clamps slot and lane into range
  std::uint64_t getTapDropCount (int slot, int lane) const;
  // existing getTapBuffer() / getTapBuffer(slot) / getTapDropCount(...) keep meaning lane 0
  ```

- [ ] **Step 1: Write the failing tests** — append to `tests/test_audioengine.cpp`, after `TapsArePerSlotWithIndependentDropCounts`. Look at that test (line ~946) for how the file builds a stereo callback with `engine.audioDeviceAboutToStart`/`audioDeviceIOCallbackWithContext`; reuse its helper names exactly.

```cpp
// Lane S: the detector needs to hear BOTH lanes. Turns red if the callback
// stops writing lane 1's post-DSP output into tapBuffers_[slot][1].
TEST (AudioEngineRouting, StereoSlotTapsBothLanesWithLaneOneCarryingLaneOneOutput)
{
    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Bypass);
    // Same device-start shape the other routing tests use (2 in, 2 out, 48 kHz, 512).
    FakeDevice device (2, 2, 48000.0, 512);
    engine.audioDeviceAboutToStart (&device);

    std::vector<float> left (512, 0.25f), right (512, -0.5f);
    const float* ins[2]  = { left.data(), right.data() };
    std::vector<float> outL (512), outR (512);
    float* outs[2] = { outL.data(), outR.data() };
    engine.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, 512, {});

    std::vector<float> tapped0 (512), tapped1 (512);
    ASSERT_EQ (engine.getTapBuffer (0, 0).read (tapped0.data(), 512), 512u);
    ASSERT_EQ (engine.getTapBuffer (0, 1).read (tapped1.data(), 512), 512u);
    EXPECT_FLOAT_EQ (tapped0[100],  0.25f);
    EXPECT_FLOAT_EQ (tapped1[100], -0.5f);   // NOT a copy of lane 0
    EXPECT_EQ (engine.getTapDropCount (0, 1), 0u);
}

// Turns red if a mono slot starts writing lane 1's ring (there is no lane 1).
TEST (AudioEngineRouting, MonoSlotLeavesLaneOneTapUntouched)
{
    AudioEngine engine;
    engine.setMode (AudioEngine::Mode::Bypass);
    FakeDevice device (2, 2, 48000.0, 512);
    engine.audioDeviceAboutToStart (&device);

    SlotConfig mono;
    mono.enabled = true; mono.width = 1;
    mono.inputChannels[0] = 0; mono.outputChannels[0] = 0;
    engine.setSlotConfig (0, mono);

    std::vector<float> left (512, 0.25f), right (512, -0.5f);
    const float* ins[2]  = { left.data(), right.data() };
    std::vector<float> outL (512), outR (512);
    float* outs[2] = { outL.data(), outR.data() };
    engine.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, 512, {});

    EXPECT_EQ (engine.getTapBuffer (0, 0).getAvailableRead(), 512u);
    EXPECT_EQ (engine.getTapBuffer (0, 1).getAvailableRead(), 0u);
    EXPECT_EQ (engine.getTapDropCount (0, 1), 0u);
}

// Turns red if the legacy accessors stop aliasing lane 0.
TEST (AudioEngineRouting, LegacyTapAccessorsAliasLaneZero)
{
    AudioEngine engine;
    EXPECT_EQ (&engine.getTapBuffer(),  &engine.getTapBuffer (0, 0));
    EXPECT_EQ (&engine.getTapBuffer (3), &engine.getTapBuffer (3, 0));
    // Out-of-range lane clamps to 0, matching the slot-clamp convention.
    EXPECT_EQ (&engine.getTapBuffer (3, 7), &engine.getTapBuffer (3, 0));
}
```
If `FakeDevice` or `setSlotConfig` are named differently in the file, use the file's names; do not invent new helpers.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Release 2>&1 | tail -3` — expected: compile error (`getTapBuffer` with two ints does not exist).

- [ ] **Step 3: Implement**

`AudioEngine.h` private members (replace the two declarations at ~268-280):
```cpp
    // Per-slot, PER-LANE post-DSP taps (lane S): [slot][lane]. The audio
    // callback is the single producer of every ring; the slot's detector
    // thread is the single consumer of BOTH of its rings.
    static constexpr size_t kTapCapacity = 8192;  // ~170 ms @ 48 kHz, power of 2
    using TapPair = std::array<LockFreeRingBuffer<float>, kMaxSlotLanes>;
    std::array<TapPair, kMaxSlots> tapBuffers_
    {{
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }},
        {{ LockFreeRingBuffer<float> (kTapCapacity), LockFreeRingBuffer<float> (kTapCapacity) }}
    }};
    std::array<std::array<std::atomic<std::uint64_t>, kMaxSlotLanes>, kMaxSlots> tapDropCounts_ {};
```
Public declarations (next to the existing ones):
```cpp
    LockFreeRingBuffer<float>& getTapBuffer (int slot, int lane);
    std::uint64_t getTapDropCount (int slot, int lane) const;
```
`AudioEngine.cpp` accessors:
```cpp
namespace {
int clampSlot (int slot) { return (slot < 0 || slot >= kMaxSlots) ? 0 : slot; }
int clampLane (int lane) { return (lane < 0 || lane >= kMaxSlotLanes) ? 0 : lane; }
}

LockFreeRingBuffer<float>& AudioEngine::getTapBuffer()               { return getTapBuffer (0, 0); }
LockFreeRingBuffer<float>& AudioEngine::getTapBuffer (int slot)      { return getTapBuffer (slot, 0); }
LockFreeRingBuffer<float>& AudioEngine::getTapBuffer (int slot, int lane)
{
    return tapBuffers_[(std::size_t) clampSlot (slot)][(std::size_t) clampLane (lane)];
}
std::uint64_t AudioEngine::getTapDropCount() const                   { return getTapDropCount (0, 0); }
std::uint64_t AudioEngine::getTapDropCount (int slot) const          { return getTapDropCount (slot, 0); }
std::uint64_t AudioEngine::getTapDropCount (int slot, int lane) const
{
    return tapDropCounts_[(std::size_t) clampSlot (slot)][(std::size_t) clampLane (lane)]
        .load (std::memory_order_relaxed);
}
```
Callback lane table: replace `const float* tapSource[kMaxSlots] {};` with `const float* tapSource[kMaxSlots][kMaxSlotLanes] {};` and replace
```cpp
            if (lane == 0)
                tapSource[slot] = out;
```
with `tapSource[slot][lane] = out;`. Tap write loop:
```cpp
    for (int slot = 0; slot < kMaxSlots; ++slot)
        for (int lane = 0; lane < kMaxSlotLanes; ++lane)
        {
            const float* src = tapSource[slot][lane];
            if (src == nullptr)
                continue;
            const size_t requested = static_cast<size_t> (numSamples);
            const size_t written   =
                tapBuffers_[(std::size_t) slot][(std::size_t) lane].write (src, requested);
            if (written < requested)
                tapDropCounts_[(std::size_t) slot][(std::size_t) lane]
                    .fetch_add (requested - written, std::memory_order_relaxed);
        }
```
Keep the existing comment block above the loop; change "lane-0" wording to "every lane". Grep `tapBuffers_\[` and `tapDropCounts_\[` across `AudioEngine.cpp` (restart-drain path uses them too) and update each to the `[slot][lane]` shape, draining both lanes.

- [ ] **Step 4: Build and run the AudioEngine suite**

Run: `cmake --build build --config Release 2>&1 | tail -3 && cd build && ctest -C Release --output-on-failure -R AudioEngine` — expected: all pass including the three new tests.

- [ ] **Step 5: Full suite, then commit**

Run: `cd build && ctest -C Release | tail -3` — expected `100% tests passed`.
```bash
git add src/app/AudioEngine.h src/app/AudioEngine.cpp tests/test_audioengine.cpp
git commit -m "feat(engine): tap both lanes of every slot into per-lane SPSC rings"
```

---

### Task 2: `NotchController` owns two `LaneAnalysis` bundles; snapshot carries both lanes

Behaviour-preserving refactor: after this task every existing test passes unchanged except the two mechanical `magnitudes` → `magnitudes[0]` edits below. Placement policy is untouched (still lane-0 detection, fan-out to `width_`).

**Files:**
- Modify: `src/app/NotchController.h` (ctor, `SnapshotBuffer`, private members)
- Modify: `src/app/NotchController.cpp` (ctor, `setNotch` sample-rate read, `runOnce`, tuning setters/getters, `setSampleRate`, `processSpectrumForDetection` member accesses)
- Modify: `src/gui/SpectrumView.cpp:630-641` (`snapshot_.magnitudes` → `snapshot_.magnitudes[0]` for now; Task 9 makes it lane-selectable)
- Modify: `tests/test_notchcontroller.cpp:354` (`snap.magnitudes.data()` → `snap.magnitudes[0].data()`)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Produces:
  ```cpp
  NotchController (LockFreeRingBuffer<float>& tapLane0,
                   LockFreeRingBuffer<float>* tapLane1,      // nullptr = legacy, LINKED
                   LockFreeRingBuffer<NotchCommand>& commands,
                   ClockSource& clock, int slotId = 0);
  // legacy ctor kept: NotchController (tap, commands, clock, slotId) -> delegates with nullptr
  struct SnapshotBuffer {
      std::array<std::array<float, Detector::kNumBins>, kChannels> magnitudes {};
      std::uint32_t magnitudeCount = 0;
      std::uint32_t laneCount = 1;
      bool          linked = false;
      double sampleRate = 0.0;
      std::array<SnapshotNotch, kTotalSlots> notches {};
      std::uint32_t notchCount = 0;
      std::uint64_t sequence = 0;
  };
  bool hasLaneOneTapForTest() const;
  ```

- [ ] **Step 1: Write the failing tests** — add to `tests/test_notchcontroller.cpp` inside the anonymous namespace (after `SlotHarness`) a stereo harness, and a test after `DefaultsMatchLegacyBehaviour`:

```cpp
// Two rings, one controller: the lane-S shape MainComponent builds.
struct StereoHarness {
    LockFreeRingBuffer<float> tapL { 8192 };
    LockFreeRingBuffer<float> tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    FakeClock clock;
    NotchController controller { tapL, &tapR, commands, clock };
};

void pumpStereo (StereoHarness& h, const std::vector<float>& left, const std::vector<float>& right)
{
    ASSERT_EQ (h.tapL.write (left.data(),  left.size()),  left.size());
    ASSERT_EQ (h.tapR.write (right.data(), right.size()), right.size());
    h.clock.advance (kBlockMs);
    h.controller.runOnce();
}
```
(`pumpStereo` must be declared after `kBlockMs`, i.e. next to `pump`.)

```cpp
// Lane S task 2. Red if the snapshot stops carrying lane 1's spectrum or
// misreports laneCount.
TEST (NotchControllerStereo, SnapshotPublishesBothLanesSpectraAndLaneCount)
{
    StereoHarness h;
    SineSource toneL;                 // 1 kHz on the left only
    NoiseSource quietR;
    for (int i = 0; i < 8; ++i)
        pumpStereo (h, toneL.hop(), quietR.hop());

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    EXPECT_EQ (snap.laneCount, 2u);
    EXPECT_FALSE (snap.linked);
    ASSERT_EQ (snap.magnitudeCount, (std::uint32_t) Detector::kNumBins);

    const int toneBin = 43;   // 1007.8 Hz @ 48 kHz / 2048
    EXPECT_GT (snap.magnitudes[0][toneBin], 10.0f * snap.magnitudes[1][toneBin]);
}

// Red if the legacy ctor reports a lane-1 tap it does not have.
TEST (NotchControllerStereo, LegacyCtorHasNoLaneOneTap)
{
    Harness h;
    EXPECT_FALSE (h.controller.hasLaneOneTapForTest());
    StereoHarness s;
    EXPECT_TRUE (s.controller.hasLaneOneTapForTest());
}

// Red if a tuning setter reaches only lane 0's analyser.
TEST (NotchControllerStereo, TuningSettersReachBothLanes)
{
    StereoHarness h;
    h.controller.setPeakinessThreshold (15.0f);
    h.controller.setRiseReferenceMs (500.0);
    EXPECT_FLOAT_EQ (h.controller.getPeakinessThreshold (0), 15.0f);
    EXPECT_FLOAT_EQ (h.controller.getPeakinessThreshold (1), 15.0f);
    EXPECT_DOUBLE_EQ (h.controller.getRiseReferenceMs (1), 500.0);
}
```
Add these lane-indexed getters to the public API alongside the existing ones (existing no-arg getters delegate to lane 0):
```cpp
    double getRiseReferenceMs (int lane) const;
    float  getPeakinessThreshold (int lane) const;
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Release 2>&1 | tail -3` — expected: compile error (no 5-argument ctor).

- [ ] **Step 3: Implement the header changes** in `src/app/NotchController.h`:

Add `#include "app/SlotConfig.h"` and `static_assert (kChannels == kMaxSlotLanes, "lanes and channels are the same axis");` right after `kTotalSlots`.

Replace the single ctor with:
```cpp
    NotchController (LockFreeRingBuffer<float>& tapLane0,
                     LockFreeRingBuffer<float>* tapLane1,
                     LockFreeRingBuffer<NotchCommand>& commands,
                     ClockSource& clock,
                     int slotId = 0);
    // Legacy shape: one tap. Behaves LINKED whatever setLinked() says (S-6):
    // with no lane-1 spectrum there is nothing to be independent about.
    NotchController (LockFreeRingBuffer<float>& tap,
                     LockFreeRingBuffer<NotchCommand>& commands,
                     ClockSource& clock,
                     int slotId = 0);

    bool hasLaneOneTapForTest() const { return taps_[1] != nullptr; }   // TEST ACCESSOR ONLY
```
Replace `SnapshotBuffer` with the struct in **Interfaces** above (keep the comment about one lock, one struct).

Private: replace `tap_`, `detector_`, `analyzer_`, `scorer_`, `persistence_`, `previousBlockNowMs_` with
```cpp
    struct LaneAnalysis
    {
        Detector          detector { 48000.0 };
        PeakinessAnalyzer analyzer;
        CandidateScorer   scorer;
        std::array<std::uint32_t, Detector::kNumBins> persistence {};
        double previousBlockNowMs = 0.0;   // <= 0: no previous block yet
    };
    std::array<LockFreeRingBuffer<float>*, kChannels> taps_ {};   // [0] never null
    std::array<LaneAnalysis, kChannels> lanes_;

    // Lanes actually analysed this run: 2 only when stereo AND a lane-1 tap exists.
    int analysedLanes() const { return (width_ == 2 && taps_[1] != nullptr) ? 2 : 1; }
```
Change `processSpectrumForDetection` to `void processSpectrumForDetection (int lane, const Detector::Spectrum& block, double blockNowMs);` (Task 3 adds the other-lane argument).

- [ ] **Step 4: Implement the .cpp changes**

Ctors:
```cpp
NotchController::NotchController (LockFreeRingBuffer<float>& tapLane0,
                                  LockFreeRingBuffer<float>* tapLane1,
                                  LockFreeRingBuffer<NotchCommand>& commands,
                                  ClockSource& clock, int slotId)
    : juce::Thread ("AZNotchDetector")
    , commands_ (commands), clock_ (clock), slotId_ (slotId)
    , lastPollMs_ (clock.nowMs())
{
    taps_[0] = &tapLane0;
    taps_[1] = tapLane1;
}

NotchController::NotchController (LockFreeRingBuffer<float>& tap,
                                  LockFreeRingBuffer<NotchCommand>& commands,
                                  ClockSource& clock, int slotId)
    : NotchController (tap, nullptr, commands, clock, slotId) {}
```
`setNotch`: `const double sampleRate = lanes_[0].detector.getSampleRate();`
`setSampleRate`: loop `for (auto& l : lanes_) l.detector.setSampleRate (sampleRate);`
Tuning: `setRiseReferenceMs` loops `lanes_[l].scorer.setRiseReferenceMs (ms)`; `setPeakinessThreshold` loops `lanes_[l].analyzer.setThreshold (t)`; getters with `int lane` read `lanes_[std::clamp (lane, 0, kChannels - 1)]`; no-arg getters call lane 0.

`runOnce()` step 1 becomes the lockstep drain (spec §4.2):
```cpp
    const double now = clock_.nowMs();
    const int lanesToRead = analysedLanes();
    for (;;)
    {
        std::array<Detector::Spectrum, kChannels> spec {};
        bool any = false;
        for (int l = 0; l < lanesToRead; ++l)
        {
            spec[(std::size_t) l] = lanes_[(std::size_t) l].detector.processLatestBlock (*taps_[(std::size_t) l]);
            any = any || spec[(std::size_t) l].magnitudes != nullptr;
        }
        if (! any)
            break;

        // --- snapshot publish: identical to today's block, but per lane ---
        std::array<SnapshotNotch, kTotalSlots> notchList {};
        std::uint32_t notchCount = 0;
        {
            const std::lock_guard<std::mutex> lock (modelMutex_);
            lastDataMs_ = now;
            for (int c = 0; c < kChannels; ++c)
                for (int i = 0; i < kSlots; ++i)
                {
                    const auto& n = model_[slotOf (c, i)];
                    if (! n.active) continue;
                    notchList[notchCount++] = { (float) n.frequency, (float) n.Q, (float) n.depthDB,
                                                (std::uint8_t) c, (std::uint8_t) i };
                }
        }
        {
            const std::lock_guard<std::mutex> lock (snapshotMutex_);
            for (int l = 0; l < lanesToRead; ++l)
                if (spec[(std::size_t) l].magnitudes != nullptr)
                    std::copy (spec[(std::size_t) l].magnitudes,
                               spec[(std::size_t) l].magnitudes + Detector::kNumBins,
                               latest_.magnitudes[(std::size_t) l].begin());
            latest_.magnitudeCount = (std::uint32_t) Detector::kNumBins;
            latest_.laneCount      = (std::uint32_t) lanesToRead;
            latest_.linked         = linked_.load (std::memory_order_relaxed);   // Task 3 adds linked_; until then write false
            latest_.sampleRate     = spec[0].magnitudes != nullptr ? spec[0].sampleRate : spec[1].sampleRate;
            latest_.notches        = notchList;
            latest_.notchCount     = notchCount;
            ++latest_.sequence;
        }

        for (int l = 0; l < lanesToRead; ++l)
            if (spec[(std::size_t) l].magnitudes != nullptr)
                processSpectrumForDetection (l, spec[(std::size_t) l], now);
    }
```
For this task, in `processSpectrumForDetection (int lane, ...)`, replace every `scorer_`, `analyzer_`, `persistence_`, `previousBlockNowMs_` with the `lanes_[(std::size_t) lane]` member, delete the `persistence_.size() != kNumBins` resize guard, and keep the rest byte-for-byte. Snapshot `linked` is written as `false` in this task (no `linked_` yet).

`SpectrumView.cpp:630-641`: `snapshot_.magnitudes.size()` → `snapshot_.magnitudes[0].size()`, `snapshot_.magnitudes[bin]` → `snapshot_.magnitudes[0][bin]`. `tests/test_notchcontroller.cpp:354`: `snap.magnitudes[0].data()`.

- [ ] **Step 5: Build, run NotchController + SpectrumView + full suite**

Run: `cmake --build build --config Release 2>&1 | tail -3 && cd build && ctest -C Release --output-on-failure -R "NotchController|SpectrumView"` then `ctest -C Release | tail -3` — expected `100% tests passed` (369 + 3 new).

- [ ] **Step 6: Commit**
```bash
git add src/app/NotchController.h src/app/NotchController.cpp src/gui/SpectrumView.cpp tests/test_notchcontroller.cpp
git commit -m "refactor(detector): per-lane analysis bundles and a two-lane snapshot, behaviour unchanged"
```

---

### Task 3: Placement policy — INDEP by default, LINK flag, lane-aware index search, asymmetry bonus

**Files:**
- Modify: `src/app/NotchController.h`, `src/app/NotchController.cpp`
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Produces:
  ```cpp
  void setLinked (bool linked);          // any thread, atomic
  bool isLinked() const;                 // raw flag
  bool effectiveLinked() const;          // flag || width_ < 2 || taps_[1] == nullptr
  void  setLaneAsymmetryBonus (float b); // clamped [1, 2], default 1
  float getLaneAsymmetryBonus() const;
  ```
- Consumes: `StereoHarness`, `pumpStereo` from Task 2.

- [ ] **Step 1: Write the failing tests** (append to `tests/test_notchcontroller.cpp`). Warm-up shape mirrors `primeAndPlace`: `kWarmupBlocks` noise blocks, then up to 40 tone blocks.

```cpp
namespace {
// Drains every command currently queued into `out`.
std::vector<NotchCommand> drain (LockFreeRingBuffer<NotchCommand>& ring)
{
    std::vector<NotchCommand> out;
    NotchCommand cmd {};
    while (ring.read (&cmd, 1) == 1)
        out.push_back (cmd);
    return out;
}

// Warm both lanes with noise, then feed `left`/`right` sources until at
// least one Set appears or 40 blocks pass. Returns everything queued.
template <typename L, typename R>
std::vector<NotchCommand> warmThenDrive (StereoHarness& h, L& left, R& right)
{
    NoiseSource quietL, quietR;
    quietR.rng.seed (999u);
    for (int i = 0; i < kWarmupBlocks; ++i)
        pumpStereo (h, quietL.hop(), quietR.hop());
    for (int i = 0; i < 40; ++i)
    {
        pumpStereo (h, left.hop(), right.hop());
        if (h.commands.getAvailableRead() > 0)
            break;
    }
    // A few more blocks so a lagging second lane (if any) gets its chance.
    for (int i = 0; i < 6; ++i)
        pumpStereo (h, left.hop(), right.hop());
    return drain (h.commands);
}
} // namespace

// Spec test 4. Red if INDEP placement fans out to the silent lane.
TEST (NotchControllerStereo, IndepHowlOnRightOnlyCutsRightOnly)
{
    StereoHarness h;
    h.controller.setDetectionActive (true);
    NoiseSource quietL;  SineSource toneR;
    const auto cmds = warmThenDrive (h, quietL, toneR);

    ASSERT_FALSE (cmds.empty());
    for (const auto& c : cmds)
    {
        EXPECT_EQ (c.type, NotchCommandType::Set);
        EXPECT_EQ (c.channel, 1) << "lane 0 must stay untouched";
    }
    EXPECT_NEAR (cmds.front().frequency, 1000.0, 0.5 * kTestSr / Detector::kFftSize);
}

// Spec test 5. Red if both-lane howl yields fewer than one Set per lane.
TEST (NotchControllerStereo, IndepHowlOnBothLanesCutsBoth)
{
    StereoHarness h;
    h.controller.setDetectionActive (true);
    SineSource toneL, toneR;
    const auto cmds = warmThenDrive (h, toneL, toneR);

    bool sawL = false, sawR = false;
    for (const auto& c : cmds) { sawL |= (c.channel == 0); sawR |= (c.channel == 1); }
    EXPECT_TRUE (sawL);
    EXPECT_TRUE (sawR);
}

// Spec test 6. Red if LINKED stops fanning a right-only howl to both lanes
// at the same index.
TEST (NotchControllerStereo, LinkedHowlOnRightOnlyCutsBothAtOneIndex)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setDetectionActive (true);
    NoiseSource quietL;  SineSource toneR;
    const auto cmds = warmThenDrive (h, quietL, toneR);

    ASSERT_GE (cmds.size(), 2u);
    EXPECT_EQ (cmds[0].channel, 0);
    EXPECT_EQ (cmds[1].channel, 1);
    EXPECT_EQ (cmds[0].index, cmds[1].index);
    EXPECT_FLOAT_EQ (cmds[0].frequency, cmds[1].frequency);
}

// Spec test 7 / S-6. Red if the legacy ctor honours INDEP.
TEST (NotchControllerStereo, LegacyCtorIsLinkedRegardlessOfFlag)
{
    Harness h;
    h.controller.setLinked (false);
    EXPECT_TRUE (h.controller.effectiveLinked());
    StereoHarness s;
    EXPECT_FALSE (s.controller.effectiveLinked());
    s.controller.setLinked (true);
    EXPECT_TRUE (s.controller.effectiveLinked());
}

// Spec test 17b / S-7. Red if LINKED reuses an index lane 1 already holds.
TEST (NotchControllerStereo, LinkedPlacementSkipsAnIndexBusyOnEitherLane)
{
    StereoHarness h;
    // Pre-occupy lane 1 index 0 (as INDEP would have) via the policy API.
    ASSERT_TRUE (h.controller.setNotch (1, 0, 3000.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.runOnce();
    (void) drain (h.commands);

    h.controller.setLinked (true);
    h.controller.setDetectionActive (true);
    SineSource toneL; NoiseSource quietR;
    const auto cmds = warmThenDrive (h, toneL, quietR);

    ASSERT_GE (cmds.size(), 2u);
    for (const auto& c : cmds)
    {
        EXPECT_EQ (c.type, NotchCommandType::Set);
        EXPECT_NE (c.index, 0) << "index 0 is busy on lane 1";
    }
}

// Spec test 8. Red if INDEP auto-release on lane 0 waits for lane 1 to go quiet.
TEST (NotchControllerStereo, IndepAutoReleaseIsPerLane)
{
    StereoHarness h;
    h.controller.setDetectionActive (true);
    SineSource toneL, toneR;
    (void) warmThenDrive (h, toneL, toneR);
    h.controller.setDetectionActive (false);   // freeze placement, keep feeding

    // Left goes quiet, right keeps ringing, for > 30 s of live time.
    NoiseSource quietL;
    const int blocks = (int) (NotchController::kAutoReleaseMs / kBlockMs) + 20;
    for (int i = 0; i < blocks; ++i)
        pumpStereo (h, quietL.hop(), toneR.hop());

    const auto cmds = drain (h.commands);
    bool clearedL = false, clearedR = false;
    for (const auto& c : cmds)
        if (c.type == NotchCommandType::Clear) { clearedL |= (c.channel == 0); clearedR |= (c.channel == 1); }
    EXPECT_TRUE (clearedL);
    EXPECT_FALSE (clearedR);
}

// Spec test 9. Red if a LINKED pair releases while one lane still rings.
TEST (NotchControllerStereo, LinkedAutoReleaseWaitsForBothLanes)
{
    StereoHarness h;
    h.controller.setLinked (true);
    h.controller.setDetectionActive (true);
    SineSource toneL; NoiseSource quietR;
    (void) warmThenDrive (h, toneL, quietR);
    h.controller.setDetectionActive (false);

    // Left goes quiet, RIGHT now rings the same frequency: the pair stays.
    NoiseSource quietL; SineSource toneR;
    const int blocks = (int) (NotchController::kAutoReleaseMs / kBlockMs) + 20;
    for (int i = 0; i < blocks; ++i)
        pumpStereo (h, quietL.hop(), toneR.hop());

    for (const auto& c : drain (h.commands))
        EXPECT_NE (c.type, NotchCommandType::Clear);
}

// Spec test 12. Red if the bonus multiplies when the other lane is NOT quiet,
// or fails to multiply when it is. Uses the public score hook below.
TEST (NotchControllerStereo, AsymmetryBonusAppliesOnlyWhenOtherLaneIsQuiet)
{
    StereoHarness h;
    EXPECT_FLOAT_EQ (h.controller.getLaneAsymmetryBonus(), 1.0f);
    h.controller.setLaneAsymmetryBonus (2.0f);
    EXPECT_FLOAT_EQ (h.controller.getLaneAsymmetryBonus(), 2.0f);
    h.controller.setLaneAsymmetryBonus (9.0f);
    EXPECT_FLOAT_EQ (h.controller.getLaneAsymmetryBonus(), 2.0f);   // clamped

    std::array<float, Detector::kNumBins> mine {}, other {};
    mine.fill (1.0f); other.fill (1.0f);
    mine[43] = 100.0f;                 // peaky here
    other[43] = 100.0f;                // other lane equally peaky -> no bonus
    EXPECT_FLOAT_EQ (NotchController::asymmetryMultiplierForTest (mine.data(), other.data(), 43, 2.0f), 1.0f);
    other[43] = 1.0f;                  // other lane flat -> bonus
    EXPECT_FLOAT_EQ (NotchController::asymmetryMultiplierForTest (mine.data(), other.data(), 43, 2.0f), 2.0f);
    EXPECT_FLOAT_EQ (NotchController::asymmetryMultiplierForTest (mine.data(), nullptr, 43, 2.0f), 1.0f);
    EXPECT_FLOAT_EQ (NotchController::asymmetryMultiplierForTest (mine.data(), other.data(), 43, 1.0f), 1.0f);
}

// Spec test 14. Red if INDEP's harmonic penalty sees the other lane's notches.
TEST (NotchControllerStereo, IndepHarmonicPenaltyIsPerLane)
{
    StereoHarness h;
    // Lock 500 Hz on lane 1 only; a 1 kHz howl on lane 0 must NOT be penalised
    // (1 kHz is 2x 500 Hz, inside the 1.4x..4.1x band).
    ASSERT_TRUE (h.controller.setNotch (1, 0, 500.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.runOnce();
    (void) drain (h.commands);
    h.controller.setDetectionActive (true);
    SineSource toneL; NoiseSource quietR;
    const auto cmds = warmThenDrive (h, toneL, quietR);
    bool setOnL = false;
    for (const auto& c : cmds) setOnL |= (c.type == NotchCommandType::Set && c.channel == 0);
    EXPECT_TRUE (setOnL) << "the lane-1 notch must not halve lane 0's score";
}
```
Public test hook (header):
```cpp
    // TEST ACCESSOR ONLY: the multiplier §4.4 applies to a candidate at `bin`.
    static float asymmetryMultiplierForTest (const float* mine, const float* other, int bin, float bonus)
        { return asymmetryMultiplier (mine, other, bin, bonus); }
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Release 2>&1 | tail -3` — expected compile errors (`setLinked` etc. undefined).

- [ ] **Step 3: Implement**

Header additions (public):
```cpp
    void setLinked (bool linked) { linked_.store (linked, std::memory_order_relaxed); }
    bool isLinked() const        { return linked_.load (std::memory_order_relaxed); }
    // LINKED behaviour is forced whenever independence is impossible (S-6).
    bool effectiveLinked() const { return isLinked() || width_ < 2 || taps_[1] == nullptr; }

    static constexpr float kMinLaneAsymmetryBonus = 1.0f;
    static constexpr float kMaxLaneAsymmetryBonus = 2.0f;
    void  setLaneAsymmetryBonus (float b);
    float getLaneAsymmetryBonus() const;
```
Private:
```cpp
    std::atomic<bool>  linked_ { false };
    std::atomic<float> laneAsymmetryBonus_ { kMinLaneAsymmetryBonus };

    static float asymmetryMultiplier (const float* mine, const float* other, int bin, float bonus);
    int  firstFreeIndexLocked (int lane) const;          // free on ONE lane
    int  firstFreeIndexAllLanesLocked() const;           // free on every lane < width_ (S-7)
    void placeConfirmed (int lane, const PeakinessAnalyzer::Candidate& cand, bool linkedNow);
    void processSpectrumForDetection (int lane, const Detector::Spectrum& block,
                                      const float* otherLaneMagnitudes, double blockNowMs);
```
Delete `firstFreeSlotLocked`.

.cpp:
```cpp
void NotchController::setLaneAsymmetryBonus (float b)
{
    laneAsymmetryBonus_.store (std::clamp (b, kMinLaneAsymmetryBonus, kMaxLaneAsymmetryBonus),
                               std::memory_order_relaxed);
}
float NotchController::getLaneAsymmetryBonus() const
{
    return laneAsymmetryBonus_.load (std::memory_order_relaxed);
}

float NotchController::asymmetryMultiplier (const float* mine, const float* other, int bin, float bonus)
{
    if (other == nullptr || ! (bonus > 1.0f))
        return 1.0f;
    if (bin < PeakinessAnalyzer::kNeighbourOuterRadius
        || bin >= Detector::kNumBins - PeakinessAnalyzer::kNeighbourOuterRadius)
        return 1.0f;
    const float minePk  = PeakinessAnalyzer::peakinessAt (mine,  Detector::kNumBins, bin);
    const float otherPk = PeakinessAnalyzer::peakinessAt (other, Detector::kNumBins, bin);
    return (otherPk < 0.5f * minePk) ? bonus : 1.0f;
}

int NotchController::firstFreeIndexLocked (int lane) const
{
    for (int i = 0; i < kSlots; ++i)
        if (! model_[slotOf (lane, i)].active)
            return i;
    return -1;
}

int NotchController::firstFreeIndexAllLanesLocked() const
{
    for (int i = 0; i < kSlots; ++i)
    {
        bool free = true;
        for (int c = 0; c < width_; ++c)
            free = free && ! model_[slotOf (c, i)].active;
        if (free)
            return i;
    }
    return -1;
}

void NotchController::placeConfirmed (int lane, const PeakinessAnalyzer::Candidate& cand, bool linkedNow)
{
    const Origin origin  = soundcheckActive() ? Origin::Soundcheck : Origin::Detector;
    const double q       = notchQ_.load (std::memory_order_relaxed);
    const double depthDb = notchDepthDb_.load (std::memory_order_relaxed);

    int index = -1, firstLane = lane, lastLane = lane;
    {
        const std::lock_guard<std::mutex> lock (modelMutex_);
        if (linkedNow) { index = firstFreeIndexAllLanesLocked(); firstLane = 0; lastLane = width_ - 1; }
        else           { index = firstFreeIndexLocked (lane); }
    }
    if (index < 0)
        return;   // chain full on the lanes concerned: same outcome as today

    int applied = 0;
    for (int l = firstLane; l <= lastLane; ++l)
        if (setNotch (l, index, cand.frequencyHz, q, depthDb, origin))
            ++applied;
    const int wanted = lastLane - firstLane + 1;
    if (applied != wanted && applied > 0)
        for (int l = firstLane; l <= lastLane; ++l)
            clearNotch (l, index);

    juce::Logger::writeToLog (
        "[detect] slot=" + juce::String (slotId_)
        + " lane=" + (linkedNow ? juce::String ("LR") : juce::String (lane == 0 ? "L" : "R"))
        + " idx=" + juce::String (index)
        + " freq=" + juce::String ((int) std::lround (cand.frequencyHz))
        + " Q=" + juce::String (q, 1) + " depth=" + juce::String (depthDb, 1)
        + " rise=" + juce::String ((int) std::lround (lanes_[(std::size_t) lane].scorer.getRiseReferenceMs())));
}
```
`processSpectrumForDetection (int lane, const Detector::Spectrum& block, const float* otherLaneMagnitudes, double blockNowMs)`:
- the auto-release feed loop and the `locked` list both iterate `c` over `[0, kChannels)` but `continue` when `! linkedNow && c != lane`;
- threshold is `la.analyzer.getThreshold()`;
- after `float score = la.scorer.scoreCandidate (...)`, add `score *= asymmetryMultiplier (block.magnitudes, otherLaneMagnitudes, cand.bin, laneAsymmetryBonus_.load (std::memory_order_relaxed));`
- the confirm branch calls `placeConfirmed (lane, cand, linkedNow);` in place of the inline fan-out block (delete that block and its `[detect]` log).
where `const bool linkedNow = effectiveLinked();` is computed once at the top.

`runOnce` detection dispatch (from Task 2) becomes:
```cpp
        for (int l = 0; l < lanesToRead; ++l)
            if (spec[(std::size_t) l].magnitudes != nullptr)
            {
                const float* other = (lanesToRead == 2 && spec[(std::size_t) (1 - l)].magnitudes != nullptr)
                                         ? spec[(std::size_t) (1 - l)].magnitudes : nullptr;
                processSpectrumForDetection (l, spec[(std::size_t) l], other, now);
            }
```
and the snapshot writes `latest_.linked = linked_.load (std::memory_order_relaxed);`.

- [ ] **Step 4: Build; run the NotchController suite; the legacy test `PersistentHowlSetsNotchOnBothChannels` must still pass unchanged** (legacy ctor ⇒ LINKED).

Run: `cmake --build build --config Release 2>&1 | tail -3 && cd build && ctest -C Release --output-on-failure -R NotchController`

If `IndepAutoReleaseIsPerLane` is flaky on the "right keeps ringing" side, raise `toneR.amp` is already 1.0; check that `lastDetectedMs` refresh uses lane 1's own spectrum for lane-1 notches. Do not loosen the assertion.

- [ ] **Step 5: Full suite, commit**

Run: `cd build && ctest -C Release | tail -3` — `100% tests passed`.
```bash
git add src/app/NotchController.h src/app/NotchController.cpp tests/test_notchcontroller.cpp
git commit -m "feat(detector): per-lane notch placement, LINK mode, lane-aware index search, asymmetry bonus"
```

---

### Task 4: `setWidth(1)` clears lane-1 notches

**Files:**
- Modify: `src/app/NotchController.cpp:41-46`
- Test: `tests/test_notchcontroller.cpp`

- [ ] **Step 1: Failing test**
```cpp
// Spec test 10. Red if narrowing to mono orphans lane-1 notches in the model.
TEST (NotchControllerSlotAware, NarrowingToMonoClearsLaneOneNotches)
{
    StereoHarness h;
    ASSERT_TRUE (h.controller.setNotch (1, 2, 800.0, 30.0, -12.0, NotchController::Origin::Manual));
    ASSERT_TRUE (h.controller.setNotch (0, 2, 800.0, 30.0, -12.0, NotchController::Origin::Manual));
    h.controller.runOnce();
    (void) drain (h.commands);

    h.controller.setWidth (1);       // thread is not running in tests: precondition holds
    h.controller.runOnce();          // flushes the outbox
    const auto cmds = drain (h.commands);
    ASSERT_EQ (cmds.size(), 1u);
    EXPECT_EQ (cmds[0].type, NotchCommandType::Clear);
    EXPECT_EQ (cmds[0].channel, 1);
    EXPECT_EQ (cmds[0].index, 2);

    NotchController::SnapshotBuffer snap;
    h.controller.copySnapshot (snap);
    for (std::uint32_t i = 0; i < snap.notchCount; ++i)
        EXPECT_EQ (snap.notches[i].channel, 0);
}
```
- [ ] **Step 2: Run** `ctest -R NarrowingToMono` after build — expected FAIL (no Clear emitted).
- [ ] **Step 3: Implement**
```cpp
void NotchController::setWidth (int lanes)
{
    // Precondition: the detector thread is STOPPED (see header comment).
    const int newWidth = std::clamp (lanes, 1, 2);
    if (newWidth < width_)
    {
        // Lanes leaving the slot take their notches with them: an active model
        // entry on a lane nobody analyses would never auto-release and would
        // keep a real filter running on a chain the operator thinks is idle.
        const std::lock_guard<std::mutex> lock (modelMutex_);
        for (int c = newWidth; c < kChannels; ++c)
            for (int i = 0; i < kSlots; ++i)
                pushClearLocked (c, i);
    }
    width_ = newWidth;
}
```
- [ ] **Step 4: Build, run `-R NotchController`, full suite.**
- [ ] **Step 5: Commit**
```bash
git add src/app/NotchController.cpp tests/test_notchcontroller.cpp
git commit -m "fix(detector): narrowing a slot to mono clears its lane-1 notches"
```

---

### Task 5: Preset `lane` and `linked`; `adoptPreset` per lane

**Files:**
- Modify: `src/app/PresetManager.h:80-108` (`PresetNotch::lane`, `PresetSlot::linked`)
- Modify: `src/app/PresetManager.cpp:136-167` (`readNotch`), `:265-290` (`readSlotEntry`), `:375-445` (`toJSON`)
- Modify: `src/app/NotchController.cpp:121-149` (`adoptPreset`)
- Test: `tests/test_presetmanager.cpp`, `tests/test_notchcontroller.cpp`

**Interfaces:**
- Produces: `PresetNotch::lane` (`int`, default `-1`), `PresetSlot::linked` (`bool`, default `false`); JSON keys `"lane"`, `"linked"`.

- [ ] **Step 1: Failing tests** — `tests/test_presetmanager.cpp` (look at `SlotIdsAndSlotConfigsSurviveARoundTrip` at ~728 for the fixture style and reuse its helper that builds a valid `Preset`):

```cpp
// Lane S / spec test 15. Red if "lane" or "linked" are dropped by the writer
// or ignored by either reader.
TEST (PresetManager, LaneAndLinkedSurviveARoundTripOnBothOverloads)
{
    Preset p = validPreset();                 // the file's existing fixture helper
    p.notches.clear();
    PresetNotch n; n.index = 3; n.freq = 1000.0; n.Q = 30.0; n.depthDB = -12.0; n.slot = 1; n.lane = 1;
    p.notches.push_back (n);
    PresetSlot s; s.index = 1; s.config.enabled = true; s.config.width = 2; s.linked = true;
    p.slots.push_back (s);

    const auto text = PresetManager::toJSON (p);
    EXPECT_TRUE (text.contains ("\"lane\""));
    EXPECT_TRUE (text.contains ("\"linked\""));

    const auto plain = PresetManager::fromJSON (text);
    ASSERT_TRUE (plain.ok) << plain.errors.joinIntoString ("\n");
    ASSERT_EQ (plain.preset.notches.size(), 1u);
    EXPECT_EQ (plain.preset.notches[0].lane, 1);
    ASSERT_EQ (plain.preset.slots.size(), 1u);
    EXPECT_TRUE (plain.preset.slots[0].linked);

    const auto aware = PresetManager::fromJSON (text, 2, 2);
    ASSERT_TRUE (aware.ok);
    EXPECT_EQ (aware.preset.notches[0].lane, 1);
    EXPECT_TRUE (aware.preset.slots[0].linked);
}

// Spec test 16 (parse half). Red if an absent "lane" stops meaning -1.
TEST (PresetManager, AMissingLaneKeyMeansEveryLane)
{
    Preset p = validPreset();
    const auto r = PresetManager::fromJSON (PresetManager::toJSON (p));
    ASSERT_TRUE (r.ok);
    for (const auto& n : r.preset.notches)
        EXPECT_EQ (n.lane, -1);
}

// Spec test 17 / S-9. Red if a bad lane is skipped instead of refusing the file,
// or if the two overloads disagree.
TEST (PresetManager, AnInvalidLaneRefusesTheWholeFileOnBothOverloads)
{
    Preset p = validPreset();
    juce::String text = PresetManager::toJSON (p);
    // Inject a bad lane into the first notch object.
    text = text.replaceFirstOccurrenceOf ("\"index\"", "\"lane\": 2, \"index\"");
    const auto plain = PresetManager::fromJSON (text);
    const auto aware = PresetManager::fromJSON (text, 2, 2);
    EXPECT_FALSE (plain.ok);
    EXPECT_FALSE (aware.ok);
    EXPECT_TRUE (plain.errors.joinIntoString ("\n").contains ("lane"));

    juce::String textStr = PresetManager::toJSON (p)
        .replaceFirstOccurrenceOf ("\"index\"", "\"lane\": \"L\", \"index\"");
    EXPECT_FALSE (PresetManager::fromJSON (textStr).ok);
}
```
If `validPreset()` does not exist under that name, use the helper the round-trip test actually uses. `toJSON` must emit `"lane"` only when `lane != -1` and `"linked"` only when true, so v1-shaped output stays v1-shaped (same rule the `slot` key follows); the round-trip test above sets both, so it sees both keys.

`tests/test_notchcontroller.cpp`:
```cpp
// Spec test 16 (adopt half) / S-8. Red if adoptPreset ignores lane, searches
// for a free index, or lets linked_ change how a file is adopted.
TEST (NotchControllerStereo, AdoptPresetHonoursLaneAndKeepsTheFilesIndex)
{
    StereoHarness h;
    h.controller.setLinked (false);
    PresetNotch both;  both.index = 5; both.freq = 1000.0; both.Q = 30.0; both.depthDB = -12.0;   // lane -1
    PresetNotch right; right.index = 7; right.freq = 2000.0; right.Q = 30.0; right.depthDB = -12.0; right.lane = 1;
    EXPECT_EQ (h.controller.adoptPreset ({ both, right }), 2);
    h.controller.runOnce();
    const auto cmds = drain (h.commands);
    ASSERT_EQ (cmds.size(), 3u);
    EXPECT_EQ (cmds[0].channel, 0); EXPECT_EQ (cmds[0].index, 5);
    EXPECT_EQ (cmds[1].channel, 1); EXPECT_EQ (cmds[1].index, 5);
    EXPECT_EQ (cmds[2].channel, 1); EXPECT_EQ (cmds[2].index, 7);
}
```
- [ ] **Step 2: Build → expected compile error on `n.lane` / `s.linked`.**
- [ ] **Step 3: Implement**

`PresetManager.h`: in `PresetNotch` add
```cpp
    // Lane S: 0 = L, 1 = R, -1 = every lane of the slot (files written before
    // lane S have no "lane" key and mean this). OPTIONAL key.
    int lane = -1;
```
in `PresetSlot` add `bool linked = false;   // OPTIONAL "linked" key, lane S`.

`readNotch`, after the `slot` block:
```cpp
    // OPTIONAL lane-S key. Absent means every lane. Present, it must be 0 or
    // 1 -- anything else is a self-contradicting file (S-9), refused in this
    // SHAPE pass so both fromJSON overloads agree.
    if (object->hasProperty ("lane"))
    {
        int lane = -1;
        if (! readWholeNumber (*object, "lane", where, errors, lane))
            ok = false;
        else if (lane != 0 && lane != 1)
        {
            errors.add (where + "\"lane\" is " + juce::String (lane) + ", expected 0 or 1");
            ok = false;
        }
        else
            out.lane = lane;
    }
```
`readSlotEntry`: `if (object->hasProperty ("linked")) ok = readBool (*object, "linked", where, errors, out.linked) && ok;`

`toJSON` notch loop: `if (notch.lane != -1) entry->setProperty ("lane", notch.lane);` — slots loop: `if (slot.linked) entry->setProperty ("linked", juce::var (true));`. Check the `anyNonDefault` predicate that gates the slots section and make `linked == true` count as non-default.

`adoptPreset`:
```cpp
int NotchController::adoptPreset (const std::vector<PresetNotch>& notches)
{
    int adopted = 0;
    for (const auto& p : notches)
    {
        // S-8: the file's index, always. A named lane lands on that lane; an
        // unnamed one lands on every lane, all-or-nothing, whatever linked_ says.
        const int firstLane = (p.lane < 0) ? 0 : p.lane;
        const int lastLane  = (p.lane < 0) ? width_ - 1 : p.lane;
        if (firstLane >= width_)
            continue;   // lane 1 named on a mono slot: not adoptable here

        int applied = 0;
        for (int lane = firstLane; lane <= lastLane; ++lane)
            if (setNotch (lane, p.index, p.freq, p.Q, p.depthDB, Origin::Preset))
                ++applied;
        const int wanted = lastLane - firstLane + 1;
        if (applied == wanted)
            ++adopted;
        else if (applied > 0)
            for (int lane = firstLane; lane <= lastLane; ++lane)
                clearNotch (lane, p.index);
    }
    return adopted;
}
```
- [ ] **Step 4: Build, `ctest -R "PresetManager|NotchController"`, full suite.**
- [ ] **Step 5: Commit**
```bash
git add src/app/PresetManager.h src/app/PresetManager.cpp src/app/NotchController.cpp tests/test_presetmanager.cpp tests/test_notchcontroller.cpp
git commit -m "feat(preset): per-notch lane and per-slot linked keys; adoptPreset honours lane"
```

---

### Task 6: `MainComponent` builds stereo controllers and carries `linked`

**Files:**
- Modify: `src/app/MainComponent.h` (add `slotLinked_`, `setSlotLinked`, `isSlotLinked`), `src/app/MainComponent.cpp:32-44` (ctor), `:390-464` (`loadPreset`)
- Test: `tests/test_gui_wiring.cpp`

**Interfaces:**
- Produces:
  ```cpp
  void setSlotLinked (int slotIndex, bool linked);   // public; SlotPanel's callback calls it (Task 7)
  bool isSlotLinked (int slotIndex) const;
  const NotchController& getControllerForTest (int slotIndex) const;
  ```

- [ ] **Step 1: Failing tests** (`tests/test_gui_wiring.cpp`, next to `RequestingAModeReachesTheEngine`; copy its `ScopedJuceInitialiser_GUI` + `MainComponent` setup):
```cpp
// Lane S. Red if the app wires controllers with one tap (legacy ctor ⇒ LINKED
// forever) or if setSlotLinked stops reaching the controller.
TEST (MainComponent, ControllersGetBothTapsAndFollowTheLinkFlag)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    for (int s = 0; s < kMaxSlots; ++s)
        EXPECT_TRUE (mc.getControllerForTest (s).hasLaneOneTapForTest()) << "slot " << s;

    EXPECT_FALSE (mc.isSlotLinked (2));
    mc.setSlotLinked (2, true);
    EXPECT_TRUE (mc.isSlotLinked (2));
    EXPECT_TRUE (mc.getControllerForTest (2).isLinked());
    EXPECT_FALSE (mc.getControllerForTest (1).isLinked());
}

// Red if loadPreset stops applying a slot's "linked" before adopting notches.
TEST (MainComponent, LoadPresetAppliesTheSlotsLinkedFlag)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    MainComponent mc;
    Preset p;
    p.version = PresetManager::CURRENT_VERSION; p.device = "test"; p.sampleRate = 48000.0; p.bufferSize = 512;
    PresetSlot s; s.index = 3; s.config.enabled = true; s.config.width = 2; s.linked = true;
    p.slots.push_back (s);
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("lane-s-linked.json");
    ASSERT_TRUE (file.replaceWithText (PresetManager::toJSON (p)));
    ASSERT_TRUE (mc.loadPreset (file));
    EXPECT_TRUE (mc.isSlotLinked (3));
    EXPECT_TRUE (mc.getControllerForTest (3).isLinked());
    file.deleteFile();
}
```
- [ ] **Step 2: Build → compile error.**
- [ ] **Step 3: Implement**

Ctor IIFE: `std::make_unique<NotchController> (engine_.getTapBuffer (i, 0), &engine_.getTapBuffer (i, 1), engine_.getCommandQueue (i), systemClock_, i);`

Header (public):
```cpp
    void setSlotLinked (int slotIndex, bool linked);
    [[nodiscard]] bool isSlotLinked (int slotIndex) const;
    [[nodiscard]] const NotchController& getControllerForTest (int slotIndex) const
        { return *notchControllers_[(std::size_t) slotIndex]; }
```
Private, right after `slotUsesGlobalTuning_`: `std::array<bool, kMaxSlots> slotLinked_ {};   // lane S: runtime source of truth; lane P reads it when SAVE exists`.

.cpp:
```cpp
void MainComponent::setSlotLinked (int slotIndex, bool linked)
{
    if (slotIndex < 0 || slotIndex >= kMaxSlots) return;
    slotLinked_[(std::size_t) slotIndex] = linked;
    notchControllers_[(std::size_t) slotIndex]->setLinked (linked);
}
bool MainComponent::isSlotLinked (int slotIndex) const
{
    return slotIndex >= 0 && slotIndex < kMaxSlots && slotLinked_[(std::size_t) slotIndex];
}
```
`loadPreset`, inside the existing `for (const auto& entry : result.preset.slots)` width-resync loop, add `setSlotLinked (entry.index, entry.linked);` before `setWidth`.

- [ ] **Step 4: Build, `ctest -R MainComponent`, full suite.**
- [ ] **Step 5: Commit**
```bash
git add src/app/MainComponent.h src/app/MainComponent.cpp tests/test_gui_wiring.cpp
git commit -m "feat(app): controllers take both lane taps; per-slot LINK state flows from presets"
```

---

### Task 7: `SlotPanel` LINK / INDEP control

**Files:**
- Modify: `src/gui/SlotPanel.h` (Row gets `SegmentedControl link`; callbacks), `src/gui/SlotPanel.cpp` (construct, seed, layout, handler)
- Modify: `src/app/MainComponent.cpp` (wire `onSlotLinkChanged` → `setSlotLinked`; `slotLinkedProvider` → `isSlotLinked`)
- Test: `tests/test_slotpanel.cpp`

**Interfaces:**
- Produces:
  ```cpp
  std::function<void (int slotIndex, bool linked)> onSlotLinkChanged;
  std::function<bool (int slotIndex)>              slotLinkedProvider;   // null -> false
  // Row: gui::SegmentedControl link { { "LINK", "INDEP" } };   index 0 = LINK, 1 = INDEP
  ```

- [ ] **Step 1: Failing tests** (`tests/test_slotpanel.cpp`, modelled on `StereoShowsBothLaneCombosMonoHidesTheSecond` at line 94 for the two-slot config provider):
```cpp
// Spec test 18. Red if the LINK control shows on a mono row, hides on a stereo
// row, or clicks stop reaching the callback with the right slot and value.
TEST (SlotPanel, LinkControlFollowsWidthAndReportsClicks)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    AudioEngine engine;
    gui::SlotPanel panel (engine);
    panel.inputChannelNamesProvider  = [] { return juce::StringArray { "In 1", "In 2" }; };
    panel.outputChannelNamesProvider = [] { return juce::StringArray { "Out 1", "Out 2" }; };
    panel.slotConfigProvider = [] (int slot)
    {
        SlotConfig c; c.enabled = true; c.width = (slot == 0) ? 1 : 2; return c;
    };
    panel.slotLinkedProvider = [] (int slot) { return slot == 1; };
    panel.refresh();
    panel.setSize (700, 400);

    EXPECT_FALSE (panel.getRowForTest (0).link.isVisible());
    EXPECT_TRUE  (panel.getRowForTest (1).link.isVisible());
    EXPECT_EQ (panel.getRowForTest (1).link.getSelectedIndex(), 0);   // LINK
    EXPECT_EQ (panel.getRowForTest (2).link.getSelectedIndex(), 1);   // INDEP

    int reportedSlot = -1; bool reportedLinked = true;
    panel.onSlotLinkChanged = [&] (int s, bool l) { reportedSlot = s; reportedLinked = l; };
    panel.getRowForTest (2).link.getSegmentForTest (0).triggerClick();
    EXPECT_EQ (reportedSlot, 2);
    EXPECT_TRUE (reportedLinked);
}
```
Use the file's real `refresh()`/provider names (check `StereoShowsBothLaneCombos...` for the exact call that seeds rows).

- [ ] **Step 2: Build → compile error (`link` member missing).**
- [ ] **Step 3: Implement**

`SlotPanel.h`: `#include "gui/SegmentedControl.h"`; in `Row` add `SegmentedControl link { { "LINK", "INDEP" } };`; add the two `std::function`s next to `onSlotTuningChanged`.

`SlotPanel.cpp` constructor loop (where `row.inLanes[lane]` are added): `addAndMakeVisible (row.link); row.link.setWantsKeyboardFocus (false); row.link.onSelected = [this, i] (int idx) { if (onSlotLinkChanged) onSlotLinkChanged (i, idx == 0); };`

Seed (in `refresh()` where `row.inLanes[1].setVisible (stereo)` is set): `row.link.setVisible (stereo); row.link.setSelectedIndex ((slotLinkedProvider && slotLinkedProvider (i)) ? 0 : 1);`

Layout in `resized()` after `row.led.setBounds (...)`: reserve a fixed `kLinkColumn = 92` column **after the LED** (so mono rows keep their geometry) and `row.link.setBounds (row.link.isVisible() ? rowArea.removeFromLeft (kLinkColumn).reduced (2, 3) : juce::Rectangle<int>());`. Add `kLinkColumn` to the captions row with caption text `LINK`. Tooltip on the control: `"LINK: one side rings, both get cut. INDEP: only the ringing side is cut. Switching does not copy existing notches."`

`MainComponent.cpp`, beside `slotPanel_.onSlotTuningChanged`: `slotPanel_.onSlotLinkChanged = [this] (int s, bool l) { setSlotLinked (s, l); }; slotPanel_.slotLinkedProvider = [this] (int s) { return isSlotLinked (s); };`

- [ ] **Step 4: Build, `ctest -R SlotPanel`, full suite.**
- [ ] **Step 5: Commit**
```bash
git add src/gui/SlotPanel.h src/gui/SlotPanel.cpp src/app/MainComponent.cpp tests/test_slotpanel.cpp
git commit -m "feat(gui): per-slot LINK/INDEP control in the routing table"
```

---

### Task 8: `NotchListPanel` LANE column

**Files:**
- Modify: `src/gui/NotchListPanel.h:100-112` (`RowText::lane`), `src/gui/NotchListPanel.cpp:12-24` (column widths), `:90-106` (row build), `:262-330` (header + row paint)
- Test: `tests/test_notchlistpanel.cpp`

- [ ] **Step 1: Failing test** (copy the setup of `TwoPublishedNotchesProduceExactlyTwoRows` at line 130, which places notches through a real controller):
```cpp
// Spec test 19. Red if the LANE column stops reading L for channel 0 and R
// for channel 1.
TEST (NotchListPanel, LaneColumnReadsLOrRPerChannel)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LockFreeRingBuffer<float> tapL { 8192 }, tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController controller { tapL, &tapR, commands, clock };
    ASSERT_TRUE (controller.setNotch (0, 0, 1000.0, 30.0, -12.0, NotchController::Origin::Manual));
    ASSERT_TRUE (controller.setNotch (1, 3, 2000.0, 30.0, -12.0, NotchController::Origin::Manual));
    std::vector<float> hop (Detector::kHopSize, 0.01f);
    tapL.write (hop.data(), hop.size()); tapR.write (hop.data(), hop.size());
    for (int i = 0; i < 5; ++i) { tapL.write (hop.data(), hop.size()); tapR.write (hop.data(), hop.size()); controller.runOnce(); }

    gui::NotchListPanel panel (controller, [] { return 0.0; });
    panel.refreshFromSnapshot();
    ASSERT_EQ (panel.rowCountForTest(), 2);
    EXPECT_EQ (panel.rowForTest (0).lane.toStdString(), "L");
    EXPECT_EQ (panel.rowForTest (1).lane.toStdString(), "R");
}
```
- [ ] **Step 2: Build → compile error (`lane` missing on `RowText`).**
- [ ] **Step 3: Implement**: `RowText` gets `juce::String lane;` (declared between `id` and `freq` in the aggregate order; update the brace-init in `refreshFromSnapshot` accordingly: `notch.channel == 1 ? "R" : "L"`). Add `constexpr float kColLaneW = 30.0f;` after `kColIdW`; shift `x1 = x0 + kColIdW + kColLaneW` and draw header `LANE` and the row's `row.lane` in the new column with the same font/colour as `#`. Update the header comment's column list.
- [ ] **Step 4: Build, `ctest -R NotchListPanel`, full suite.**
- [ ] **Step 5: Commit**
```bash
git add src/gui/NotchListPanel.h src/gui/NotchListPanel.cpp tests/test_notchlistpanel.cpp
git commit -m "feat(gui): LANE column in the active-notch table"
```

---

### Task 9: `SpectrumView` display-lane selector and lane-1 marker style

**Files:**
- Modify: `src/gui/SpectrumView.h` (`laneGroup_`, `displayLane_`, `setDisplayLane`, `getDisplayLane`), `src/gui/SpectrumView.cpp` (ctor, `resized`, `rebuildGeometry`, marker pass 2)
- Test: `tests/test_spectrumview.cpp`

**Interfaces:**
- Produces: `void setDisplayLane (int lane); [[nodiscard]] int getDisplayLane() const;` and `SegmentedControl laneGroup_ { { "L", "R" } };` (exposed via `[[nodiscard]] SegmentedControl& getLaneGroupForTest()`).

- [ ] **Step 1: Failing test** (modelled on `RefreshSeesThePublishedSequenceAndPaintDoesNotCrash`, line 70):
```cpp
// Spec test 20. Red if choosing R still plots lane 0, or if the selector is
// enabled for a mono controller.
TEST (SpectrumView, LaneSelectorPlotsTheChosenLaneAndIsDisabledForMono)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    LockFreeRingBuffer<float> tapL { 8192 }, tapR { 8192 };
    LockFreeRingBuffer<NotchCommand> commands { 128 };
    JuceMonotonicClock clock;
    NotchController stereo { tapL, &tapR, commands, clock };

    // Loud left, quiet right, a few hops so both detectors publish.
    std::vector<float> loud (Detector::kHopSize, 0.5f), quiet (Detector::kHopSize, 0.0005f);
    for (int i = 0; i < 6; ++i) { tapL.write (loud.data(), loud.size()); tapR.write (quiet.data(), quiet.size()); stereo.runOnce(); }

    gui::SpectrumView view (stereo);
    view.setSize (800, 400);
    view.refreshFromSnapshot();
    EXPECT_TRUE (view.getLaneGroupForTest().isEnabled());
    EXPECT_EQ (view.getDisplayLane(), 0);
    const auto leftPoint = view.spectrumPointForTest (0);

    view.setDisplayLane (1);
    view.refreshFromSnapshot();
    const auto rightPoint = view.spectrumPointForTest (0);
    EXPECT_GT (rightPoint.y, leftPoint.y) << "quiet lane plots lower (y grows downward)";

    LockFreeRingBuffer<float> tapMono { 8192 };
    NotchController mono { tapMono, commands, clock };
    view.setController (mono);
    view.refreshFromSnapshot();
    EXPECT_FALSE (view.getLaneGroupForTest().isEnabled());
    EXPECT_EQ (view.getDisplayLane(), 0);
}
```
- [ ] **Step 2: Build → compile error.**
- [ ] **Step 3: Implement**

Header: `SegmentedControl laneGroup_ { { "L", "R" } };`, `int displayLane_ = 0;`, public `setDisplayLane`/`getDisplayLane`/`getLaneGroupForTest`.

Ctor: `laneGroup_.onSelected = [this] (int idx) { setDisplayLane (idx); }; laneGroup_.setWantsKeyboardFocus (false); addAndMakeVisible (laneGroup_);` (add it to the `for (auto* group : {...})` list instead if simpler).

`setDisplayLane`: `displayLane_ = juce::jlimit (0, 1, lane); laneGroup_.setSelectedIndex (displayLane_); drawnSequence_ = 0; rebuildGeometry(); repaint();`

`refreshFromSnapshot`, after the copy: `const bool stereo = snapshot_.laneCount >= 2; laneGroup_.setEnabled (stereo); if (! stereo && displayLane_ != 0) { displayLane_ = 0; laneGroup_.setSelectedIndex (0); }`. `setController` resets `displayLane_ = 0`.

`rebuildGeometry`: read `snapshot_.magnitudes[(std::size_t) displayLane_]`.

`resized()`: `place (laneGroup_);` immediately after `place (bandGroup_)`.

Marker pass 2, for `notch.channel == 1`: draw the colour stem with a dashed stroke instead of `drawLine`:
```cpp
        if (notch.channel == 1)
        {
            const float dashes[] = { 4.0f, 3.0f };
            markerPath_.clear();
            markerPath_.startNewSubPath (stemX, top);
            markerPath_.lineTo (stemX, plot.getBottom());
            juce::Path dashed;                       // local, small: allocation here is acceptable ONLY if
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, markerPath_, dashes, 2);   // the no-alloc paint test still passes; otherwise pre-reserve a member path
            g.strokePath (dashed, juce::PathStrokeType (1.0f));
        }
        else
            g.drawLine (stemX, top, stemX, plot.getBottom(), 1.0f);
```
and append `"R"` after the flag digits for channel 1 (widen `flagW` by one digit when `notch.channel == 1`). Run `HundredPaintsOnAnUnchangedBufferDoNotGrowMemberContainers`; if the local `dashed` path violates it, promote `dashed` to a member `dashedStemPath_` reserved in the ctor.

- [ ] **Step 4: Build, `ctest -R SpectrumView`, full suite.**
- [ ] **Step 5: Commit**
```bash
git add src/gui/SpectrumView.h src/gui/SpectrumView.cpp tests/test_spectrumview.cpp
git commit -m "feat(gui): L/R display-lane selector and dashed lane-1 notch stems"
```

---

### Task 10: Screenshot, docs, release to alpha

**Files:**
- Modify: `docs/KY-THUAT-CHONG-HU.md` (§1 diagram: tap ×2 per slot; §2 step 3; new §3.4 paragraph on lane policy + LINK; §4 keys `lane`/`linked`; §5 table row; §7 status line), `docs/GIOI-THIEU.md` (feature bullet: stereo independent + LINK)
- Modify: `CMakeLists.txt` version (by the release script, not by hand)

- [ ] **Step 1: Render the console** with a stereo INDEP slot holding an L and an R notch at different frequencies and a LINK slot. Read `.claude/skills/juce-component-snapshot/SKILL.md` for how the snapshot tool seeds state; if it cannot seed notches, drive `MainComponent::getControllerForTest`-style placement through the tool's existing seeding hook, or extend the tool minimally (document what you added).

Run: `build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast` then open `shots/console-live.png` with the Read tool and confirm: LANE column shows L and R, a dashed stem with an R tag on the plot, the LINK/INDEP control on stereo rows only, the L/R selector in the analyser toolbar. Fix anything illegible before continuing.

- [ ] **Step 2: Update the two docs.** In `KY-THUAT-CHONG-HU.md` §3.4 add:

> **Theo làn (lane S, 2026-09-05).** Slot stereo có hai bộ phân tích (Detector + PeakinessAnalyzer + CandidateScorer) chạy lockstep trên cùng một detector thread, mỗi làn tự đếm persistence và tự đặt notch trên làn của mình (INDEP, mặc định). LINK trên SlotPanel trả về hành vi cũ: một làn confirm → cả hai làn nhận notch tại index rảnh ở cả hai làn. Auto-release theo (làn, index); khi LINK, cặp nhả cùng lúc khi cả hai làn im. Tham số `laneAsymmetryBonus` (mặc định 1.0, chưa có núm GUI) chờ sweep ở lane T. Ctor một tap (test cũ) luôn LINK.

and in §4: "Notch có key tùy chọn `lane` (0/1; thiếu = mọi làn); slot có key tùy chọn `linked`. `lane` sai → từ chối cả file." Update §5 table with "Notch mồ côi làn 1 khi thu về mono | `setWidth` push Clear cho làn rời slot". Update §1 mermaid: `TAP["16 tap ring buffer SPSC<br/>(2 làn × 8 slot)"]`.

In `GIOI-THIEU.md` add under features: "**Stereo độc lập**: mic hú qua loa trái thì chỉ cắt cánh trái; nút LINK mỗi slot để cắt cả hai bên như trước."

- [ ] **Step 3: Commit docs**
```bash
git add docs/KY-THUAT-CHONG-HU.md docs/GIOI-THIEU.md
git commit -m "docs: stereo-aware detection, LINK mode, lane preset keys"
```

- [ ] **Step 4: Release** (minor bump: default behaviour on stereo slots changed)

Run: `pwsh -File installer\release-alpha.ps1 -Part minor` — expected: ctest gate green, installer copied to the drop folder, version 1.1.0. Then commit the bumped `CMakeLists.txt`:
```bash
git add CMakeLists.txt
git commit -m "chore: release 1.1.0 to alpha (stereo-aware detection)"
```

- [ ] **Step 5: Tester note** — write `docs/release-notes/1.1.0-alpha.md` with spec §3 verbatim (expected level change) and the instruction "Thấy ring dai một bên trong khi bên kia đã cắt → bật LINK cho slot đó và báo lại." Commit it with the release commit or immediately after.

---

## Self-review

- **Spec coverage:** §4.1 → Task 1; §4.2 → Task 2; §4.3 → Tasks 3, 4, 5; §4.4 → Task 3; §4.5 → Task 2; §4.6 → Task 5; §4.7 → Tasks 7, 8, 9; §4.8 → Tasks 6, 7; §4.9 → Task 10; §5 tests 1-3 → T1, 4-9/12/14/17b → T3, 10 → T4, 11/13 → T2, 15-17 → T5, 16-adopt → T5, 18 → T7, 19 → T8, 20 → T9; §6 → Task 10.
- **Placeholders:** none; every code step carries code. The only judgement calls left to the implementer are fixture helper names that already exist in the test files (named where known).
- **Type consistency:** `taps_` is `std::array<LockFreeRingBuffer<float>*, kChannels>`; `lanes_` is `std::array<LaneAnalysis, kChannels>`; `SnapshotBuffer::magnitudes` is `std::array<std::array<float, kNumBins>, kChannels>` (Task 2) and read as `magnitudes[displayLane_]` (Task 9) / `magnitudes[0]` (Task 2 consumers); `effectiveLinked()` (Task 3) is what Task 5's `adoptPreset` deliberately does **not** consult (S-8); `setSlotLinked` (Task 6) is what Task 7's callback calls.
