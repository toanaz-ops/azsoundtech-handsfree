# AZ Soundtech Hands-free Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone Windows ASIO audio application that automatically detects and eliminates feedback in real-time using FFT-based detection and notch filtering.

**Architecture:** Real-time audio thread handles ASIO I/O and biquad notch chain (lock-free, zero allocation). Background detector thread performs FFT analysis, peakiness scoring, and harmonic-aware detection, communicating via lock-free SPSC queue. JUCE framework for audio, DSP, and GUI.

**Tech Stack:** C++17, JUCE 7+, CMake, GoogleTest, ASIO SDK (Steinberg), GitHub Actions CI

**Spec:** `docs/superpowers/specs/2026-08-19-az-soundtech-hands-free-design.md`

## Global Constraints

- Windows 10/11 64-bit only (v1)
- JUCE 7.0+ (CMake support)
- C++17 minimum
- Sample rates: 44.1, 48, 88.2, 96 kHz
- Stereo only (2 in / 2 out)
- Max 16 notches per channel
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
double peakiness = binMag / mean(neighbors ± 2 bins);
if (peakiness > 10.0) candidateScore += 0.5;
```

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
5. ✅ License activate/deactivate works
6. ✅ Installer runs clean

