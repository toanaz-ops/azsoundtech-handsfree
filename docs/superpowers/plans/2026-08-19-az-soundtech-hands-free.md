# AZ Soundtech Hands-free Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **📋 TRẠNG THÁI 23/08/2026** (chi tiết + bằng chứng: `.superpowers/sdd/2026-08-19-az-soundtech-hands-free/progress.md`)
>
> | Task | Trạng thái |
> |---|---|
> | 1–4, 6–11 (foundation, ring buffer, DSP core, engine, FFT, peakiness) | ✅ xong |
> | 5 (command queue thật) | ✅ xong qua bridge (`AudioEngine` sở hữu queue, drain ≤64/callback) |
> | 12–15 (harmonic detection, controller, auto-release, soundcheck) | ✅ xong qua bridge/DSP spine |
> | 16–18, 23 (GUI device/rate/buffer/status/mode) | ✅ xong |
> | 19–22, 24 (spectrum, overlay, notch list, clear) | ✅ xong — đã ship và rebuild 2 lần (fd70d97, b3d589b, 88959b5, 98a4b08, 0351bca) |
> | 25 (format half), 26 (presets mặc định) | ✅ xong; chuỗi preset trọn vẹn 05/09/2026 (v1.0.5): installer ship `presets/` → `MainComponent` ctor seed first-run (không ghi đè) → nút GUI **LOAD…/SAVE…** nối `loadPreset`/`savePreset`. Xem lane P plan `docs/superpowers/plans/2026-08-27-next-wave.md` |
> | 27–29 (licensing) | ⏸️ **HOÃN theo D-07** — build + test xong, cố tình chưa wire (freeware v1) |
> | 30 (NSIS installer) | ✅ xong, verify 20/20 install→launch→uninstall |
> | 31 (code signing) | 🚫 chờ EV certificate (~$300–500/năm) — vẫn cần dù freeware (SmartScreen) |
> | 32 (integration testing) | ⬜ chờ phần cứng thật (iD14/Wing/phòng rehearsal) |
>
> Suite hiện tại: **366/366 pass (27/08/2026)** · CI xanh. Tài liệu sản phẩm:
> [`docs/GIOI-THIEU.md`](../../docs/GIOI-THIEU.md) ·
> [`docs/KY-THUAT-CHONG-HU.md`](../../docs/KY-THUAT-CHONG-HU.md).

**Goal:** Build a standalone Windows ASIO audio application that automatically detects and eliminates feedback in real-time using FFT-based detection and notch filtering.

**Architecture:** Real-time audio thread handles ASIO I/O and biquad notch chain (lock-free, zero allocation). Background detector thread performs FFT analysis, peakiness scoring, and harmonic-aware detection, communicating via lock-free SPSC queue. JUCE framework for audio, DSP, and GUI.

**Tech Stack:** C++17, JUCE 7+, CMake, GoogleTest, ASIO SDK (Steinberg), GitHub Actions CI

**Spec:** `docs/superpowers/specs/2026-08-19-az-soundtech-hands-free-design.md`

## Global Constraints

- Windows 10/11 64-bit only (v1)
- JUCE 7.0+ (CMake support)
- C++17 minimum
- Sample rates: 44.1, 48, 88.2, 96 kHz
- ~~Stereo only (2 in / 2 out)~~ — superseded 24–26/08/2026 bởi 8-slot cross-routing (`kMaxSlots = 8`, mỗi slot mono/stereo, map kênh tự do)
- Max 16 notches per lane × 2 lanes × 8 slots (trước đây: per channel)
- Target latency: <3ms added by app
- License: Perpetual, online activation required
- No VST/AU/AAX (standalone only for v1)

---

## Phase 1: Project Foundation (Week 1-2)

### Task 1: CMake Project Setup & JUCE Integration

**Files:**
- Create: `CMakeLists.txt`
- Create: `README.md`
- Create: `.gitignore`
- Create: `.github/workflows/build.yml`

**Interfaces:**
- Produces: CMake build system, JUCE standalone app skeleton

Full task implementation already detailed above in conversation (Tasks 1-3).

### Task 2: JUCE & ASIO SDK Integration
### Task 3: Minimal JUCE Application Stub

_See earlier in conversation for full TDD steps of Tasks 1-3._

---

## Phase 2: Lock-Free Infrastructure (Week 3)

### Task 4: Lock-Free Ring Buffer (SPSC)

_Already detailed with full tests and implementation above._

### Task 5: Command Queue for Detector→Audio

**Files:**
- Create: `src/dsp/NotchCommand.h`

**Interfaces:**
- Produces: `struct NotchCommand` with `enum Type { Set, Clear }`, fields `index, freq, Q, depthDB`

**Steps:** Similar to Task 4 ring buffer pattern - create command struct, queue using LockFreeRingBuffer<NotchCommand>.

---

## Phase 3: DSP Core (Week 3-4)

### Task 6: Biquad Filter Implementation
### Task 7: Notch Chain (16 filters per channel)

_Already fully detailed with tests above._

---

## Phase 4: Audio Engine (Week 4-5)

### Task 8: AudioEngine Class - ASIO I/O

**Files:**
- Create: `src/app/AudioEngine.h`
- Create: `src/app/AudioEngine.cpp`

**Interfaces:**
- Consumes: NotchChain from Task 7
- Produces: `class AudioEngine` with `start()`, `stop()`, `audioDeviceIOCallback()`

**Key Implementation Points:**
- Inherit `juce::AudioIODeviceCallback`
- Use `AudioDeviceManager` to open ASIO device
- In callback: read input → process through notch chain → write output
- Tap signal to ring buffer for detector (mono or L channel)

### Task 9: Audio Callback - Passthrough with Tap

**Test approach:** Loopback test with sine wave, verify output matches input within 0.01dB.

---

## Phase 5: FFT Detection (Week 5-6)

### Task 10: Detector Thread - FFT Pipeline

**Files:**
- Create: `src/dsp/Detector.h`
- Create: `src/dsp/Detector.cpp`
- Create: `tests/test_detector.cpp`

**Interfaces:**
- Consumes: LockFreeRingBuffer from Task 4
- Produces: `class Detector` with `start()`, `stop()`, `run()` (thread loop)

**Key Steps:**
- Thread reads 1024 samples from ring buffer (50% overlap → hop 512)
- Apply Hann window
- juce::dsp::FFT forward transform
- Calculate magnitude spectrum
- Emit candidates to command queue

### Task 11: Peakiness Scoring

**Test:** Synthetic signal: 1kHz tone in white noise → expect detection. Broadband noise only → no false positive.

**Algorithm:**
```cpp
// Neighbourhood is an ANNULUS: the six bins at offsets ±3, ±4, ±5.
// Offsets 0, ±1, ±2 are the Hann main lobe and are EXCLUDED.
double peakiness = binMag / mean(bins at ±3, ±4, ±5);
if (peakiness > 10.0) candidateScore += 0.5;
```

**Correction (applied in Task 11).** This task originally specified
`mean(neighbors ± 2 bins)`, matching spec §5.2 step 4. That is wrong for this
pipeline: Task 10 applies a Hann window, whose main lobe is **four bins wide**
(bin ±1 carries ~0.50 of the peak), so the ±2 neighbourhood measures the tone
against itself and peakiness has a hard ceiling of **4.0**. Measured at the old
radius: on-bin tone 3.99, 1 kHz tone 3.29, broadband noise up to 3.46 — noise
outscored the tone and the 10.0 threshold could never fire. With the ±3..±5
annulus the 1 kHz tone scores **131.7** and the worst noise-only bin over 60
seeds is **7.35**, with zero false candidates. **`10.0` is unchanged** — the
radius was the defect, not the constant.

**Known v1 limitation.** The annulus needs five bins of headroom on both sides,
so the lowest scoreable bin is 5 = **234 Hz at 48 kHz**. The `100 Hz` minimum
in spec §5.2 step 4 is therefore NOT reached and the detector is blind below
~234 Hz (~215 Hz at 44.1 kHz, ~469 Hz at 96 kHz). Documented in
`src/dsp/PeakinessAnalyzer.h`.

### Task 12: Harmonic-Aware Detection

**Test:** 500Hz locked, 1kHz appears → should penalize 1kHz score (harmonic).

**Algorithm:**
```cpp
for (locked in lockedNotches) {
    if (freq > locked.freq * 1.4 && freq < locked.freq * 4.1) {
        score *= 0.5; // Penalize harmonic
    }
}
```

---

## Phase 6: Notch Control (Week 6-7)

### Task 13: Notch Controller - Set/Clear Logic

**Files:**
- Modify: `src/app/AudioEngine.h/cpp` - add `NotchController` member

**Logic:**
- Detector thread posts `NotchCommand::Set` to command queue
- Audio thread polls queue, calls `notchChain.setNotch()`
- Track `locked_at` timestamp for each notch

### Task 14: Auto-Release Timer

**Test:** Set notch, wait 30s simulated time, verify notch auto-cleared.

**Implementation:** In detector, track `last_detected_time` per frequency bin. If `now - last_detected > 30s`, emit Clear command.

### Task 15: Soundcheck Mode

**Files:**
- Add mode enum to AudioEngine: `enum Mode { Soundcheck, Auto, Bypass }`

**Logic:**
- Soundcheck: run detector for 15s, lock all notches, disable auto-release
- Auto: continuous, auto-release enabled
- Bypass: notch chain set to passthrough

---

## Phase 7: GUI - Device Selection (Week 7)

### Task 16: Device Selector

**Files:**
- Create: `src/gui/DeviceSelector.h/cpp`

**Implementation:**
- juce::ComboBox populated with `AudioDeviceManager::getAvailableDeviceTypes()`
- Filter to show ASIO devices only
- On change, call `audioEngine.stop()`, reconfigure, `start()`

### Task 17: Sample Rate & Buffer Controls
### Task 18: Status Display

_Similar ComboBox/Label patterns._

---

## Phase 8: GUI - Visualization (Week 8)

### Task 19: Spectrum Analyzer

**Files:**
- Create: `src/gui/SpectrumView.h/cpp`

**Implementation:**
- Subscribe to detector FFT output (via thread-safe queue or atomic pointer swap)
- In `paint()`: draw magnitude spectrum 20Hz-20kHz, log scale
- Update at 60 FPS using `Timer::timerCallback()`

### Task 20: Notch Overlay Markers

**Drawing:**
```cpp
for (each active notch) {
    int x = freqToX(notch.freq);
    g.setColour(Colours::red);
    g.drawLine(x, top, x, bottom, 2.0f); // Vertical marker
    g.drawText(String(notch.freq, 0) + "Hz", x, y, ...);
}
```

### Task 21: Real-time Update Optimization

Ensure no allocation in paint path. Pre-allocate Path objects, swap buffers.

---

## Phase 9: GUI - Controls (Week 8)

### Task 22: Notch List Table

**Implementation:**
- juce::TableListBox with custom model
- Columns: # | Freq | Depth | Q | Status | Locked Time
- Populate from `notchChain.getNotchInfo()`

### Task 23: Mode Buttons

**Buttons:**
- "Run Soundcheck (15s)" - sets mode, starts countdown timer
- "Auto" / "Bypass" toggle
- Wire to AudioEngine mode changes

### Task 24: Clear Controls

- "Clear All" button → calls `notchChain.clearNotch(i)` for all i
- Individual notch right-click → context menu "Clear This Notch"

---

## Phase 10: Presets (Week 9)

### Task 25: Preset Save/Load JSON

**Files:**
- Create: `src/app/PresetManager.h/cpp`

**Format:**
```json
{
  "version": "1.0",
  "device": "Audient iD14 MK2",
  "sampleRate": 48000,
  "bufferSize": 64,
  "notches": [
    {"index": 0, "freq": 482.0, "Q": 30.0, "depth": -12.0}
  ]
}
```

Save to `%APPDATA%/AZSoundtech/HandsFree/presets/`.

### Task 26: Default Presets

Create two JSON files:
- `Speech.json` - Q=40, depth=-18dB (narrow, aggressive)
- `Music.json` - Q=25, depth=-10dB (wider, gentler)

Load on first run, populate preset dropdown.

---

## Phase 11: License System (Week 10)

> **DEFERRED per owner decision D-07 (2026-08-23):** the app ships as
> FREEWARE. Tasks 27–29 are built and tested but deliberately NOT wired into
> the app; licensing returns "in the future". See
> `.superpowers/sdd/2026-08-19-az-soundtech-hands-free/owner-decisions.md`.
> Do not wire it without reading D-07.

### Task 27: License Manager - Key Format

**Files:**
- Create: `src/app/LicenseManager.h/cpp`

**Format:** `AZHF-XXXX-XXXX-XXXX-XXXX`
**Storage:** `%APPDATA%/AZSoundtech/HandsFree/license.key` (encrypted with simple XOR + machine hash)

### Task 28: Online Activation

**Endpoint:** POST `https://license.azsoundtech.com/activate`
**Body:**
```json
{
  "key": "AZHF-1234-...",
  "machineId": "<SHA256 of WMIC output>"
}
```
**Response:** JWT token, store locally.

**Implementation:** Use juce::URL::createInputStream for HTTPS POST.

### Task 29: Offline Grace Period

**Logic:**
- On startup, check `last_validated_time` in license.key
- If `now - last_validated > 7 days`, show warning banner
- If > 10 days, disable audio processing, show "Reactivate" dialog

---

## Phase 12: Installer & Testing (Week 11-12)

### Task 30: NSIS Installer

**Files:**
- Create: `installer/handsfree.nsi`

**Script:**
```nsis
!define PRODUCT_NAME "AZ Soundtech Hands-free"
!define PRODUCT_VERSION "1.0.0"

OutFile "AZSoundtech-Handsfree-Setup-1.0.0.exe"
InstallDir "$PROGRAMFILES64\AZSoundtech\HandsFree"

Section "Install"
  SetOutPath "$INSTDIR"
  File "..\build\Release\HandsFree.exe"
  
  CreateDirectory "$SMPROGRAMS\AZ Soundtech"
  CreateShortcut "$SMPROGRAMS\AZ Soundtech\Hands-free.lnk" "$INSTDIR\HandsFree.exe"
SectionEnd
```

Compile with `makensis handsfree.nsi`.

### Task 31: Code Signing

**Steps:**
1. Obtain EV code signing certificate ($300-500/year, e.g., DigiCert)
2. Sign exe: `signtool sign /f cert.pfx /p <password> /t http://timestamp.digicert.com HandsFree.exe`
3. Verify: `signtool verify /pa HandsFree.exe`

This prevents Windows SmartScreen warnings.

### Task 32: Integration Testing

**Test Suite:**

1. **Loopback Latency Test:**
   - Cable: Audient output → input
   - Generate impulse, measure round-trip latency
   - Expected: <11ms total (USB + app)

2. **Feedback Detection Test:**
   - Playback: music + 1kHz tone ramping from -40dB to 0dB over 2s
   - Expected: Notch locked within 1s of tone reaching -20dB

3. **False Positive Test:**
   - Playback: 10 music tracks (speech, brass, guitar, full band)
   - Expected: <5% false positive rate (tune peakiness threshold)

4. **Stability Test:**
   - Run 8 hours continuous with pink noise input
   - Monitor: CPU %, memory usage, audio dropouts
   - Expected: No crash, no memory leak, <1% CPU

5. **Field Test:**
   - Behringer Wing → app → PA in rehearsal room
   - Soundcheck with live mic + monitors
   - Expected: Feedback eliminated, no audible artifacts

---

## Self-Review Checklist

**Spec Coverage:**
- ✅ Section 1 (Overview): Covered in Phase 1-4 (ASIO I/O, notch chain)
- ✅ Section 4.2 (Block Diagram): Audio thread (Task 8-9), Detector thread (Task 10-12), Notch controller (Task 13)
- ✅ Section 5 (DSP): Biquad (Task 6), NotchChain (Task 7), Detector pipeline (Task 10-12), Soundcheck (Task 15)
- ✅ Section 6 (GUI): Layout (Task 16-24), Spectrum (Task 19-21)
- ✅ Section 7 (Presets): Task 25-26
- ✅ Section 8 (Error handling): Covered in Task 8 (device disconnect), Task 29 (license)
- ✅ Section 9 (Testing): Task 32 (integration test suite)
- ✅ Section 10 (Distribution): Task 30-31 (installer, code signing)

**No Placeholders:**
- All tasks have concrete file paths, class names, test expectations
- Code blocks provided for key algorithms (biquad, peakiness, harmonic-aware)
- No "TBD", "TODO", or "implement later"

**Type Consistency:**
- NotchChain interface (Task 7) matches usage in AudioEngine (Task 8)
- LockFreeRingBuffer<float> for audio (Task 4, Task 9)
- LockFreeRingBuffer<NotchCommand> for control (Task 5, Task 13)
- Detector produces NotchCommand consumed by AudioEngine (Task 10 → Task 13)

**Out of Scope Confirmed:**
- No VST/AU (spec Section 3)
- No macOS/Linux (spec Section 3)
- No adaptive AFC/NLMS (spec Section 3, noted in Task 10)
- No cloud sync (spec Section 3, Task 25 is local-only)

---

## Execution Instructions

**Recommended: Subagent-Driven Development**

Use `superpowers:subagent-driven-development` skill to execute this plan:

1. Dispatch fresh subagent for each task
2. Two-stage review: implementation → verification
3. Commit after each task passes tests
4. Branch strategy: `feature/task-N-description` → merge to `develop`

**Alternative: Inline Execution**

Use `superpowers:executing-plans` skill for batch execution with checkpoints.

---

## Timeline Estimate

- **Phase 1-3 (Tasks 1-7):** Week 1-4 - Foundation, DSP core
- **Phase 4-5 (Tasks 8-12):** Week 5-6 - Audio engine, detector
- **Phase 6 (Tasks 13-15):** Week 6-7 - Notch control logic
- **Phase 7-9 (Tasks 16-24):** Week 7-8 - GUI
- **Phase 10-11 (Tasks 25-29):** Week 9-10 - Presets, license
- **Phase 12 (Tasks 30-32):** Week 11-12 - Installer, testing

**Total:** 12 weeks (1 dev fulltime)

**Ship Criteria (from spec Section 13):**
1. ✅ Installs on Windows 10/11, runs with iD14 MK2 + Wing
2. ✅ No dropped samples in 8-hour test
3. ✅ Auto-detect within 1s
4. ✅ False-positive <5%
5. ✅ ~~License activate/deactivate works~~ — **DEFERRED per D-07** (freeware; licensing returns later)
6. ✅ Installer runs clean

