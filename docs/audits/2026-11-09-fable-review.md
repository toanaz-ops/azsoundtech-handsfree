# Whole-project review 9835a48 (Fable)

- Date: 2026-11-09 · Model: claude-fable-5-1 · Method: read-only, no build/test run
- Scope: `caf8b66..9835a48` (lane U Dumb/Geek + wizard + input meter, lane F/P1 sub-bin notch frequency, lane L1 offline copilot, detector sample-rate fix, driver-restart hooks, CI sanitizers). Prior audits `docs/audits/2026-10-21-fable-audit.md` and `docs/superpowers/audits/2026-10-20-full-project-review.md` read first; nothing fixed there is repeated.

## 0. Verdict

No BLOCKER, no HIGH. The audio callback (`src/app/AudioEngine.cpp:636-1083`) is still allocation- and lock-free; the two new blocks on it are the input meter (`:692-709`, relaxed atomics, bounded by `jmin(numIn, kInputMeterChannels)`, null-pointer skip, reads only) and `NotchChain::healState()` (`:916-920`, `src/dsp/NotchChain.cpp:195-212`, alloc-free replay of the stored design). Sub-bin placement is correct and well-bounded. Lane U is functionally sound but has one memory-loss bug and a widened blast radius around `changeSlotConfig`. Three MEDIUM, nine LOW.

## 1. Items briefed as "open" that are already fixed on main

| Item | Where fixed |
|---|---|
| Detector-thread per-block allocations | `NotchController.cpp:1484-1485` stack `std::array`; `:1602-1628` `pendingScratch_` swap, reserved in ctor `:21-22` |
| 1 s `stopThread` timeouts | `NotchController.cpp:52` floors to `kMinStopTimeoutMs = 5000` (`.h:698`) + `jassert(stopped)`; `SoundcheckController.cpp:375-377` same |
| Soundcheck reference FFT per target | `LoopGainEstimator.cpp:50-66` `resetKeepingReference`; `SoundcheckController.cpp:819-820`, `:244` |
| Ramp-out stuck pending on unmapped channel | `AudioEngine.cpp:993-999` |
| `AudioEngine::stop()` no-op after error | `AudioEngine.cpp:95-111` keys on `callbackRegistered_` |
| Scorer live threshold / Bypass sum / commands survive restart / NaN heal / Q clamp / rate / hooks | `CandidateScorer.cpp:66-74`; `AudioEngine.cpp:881-882`; `:1169-1186`; `:916-920`; `NotchController.cpp:221`; `MainComponent.cpp:644`,`:1323`; `AudioEngine.cpp:1188-1197` + `MainComponent.cpp:684-717` |

Still open: (a) eight detector threads start unconditionally after `engine_.start()` — `MainComponent.cpp:1305`, `:1319-1326`, no `engine_.isRunning()` gate (LOW, agent-safe yes); (b) double device open on type switch — `AudioEngine.cpp:59-67` (LOW, agent-safe no: needs an ASIO driver).

## 2. Findings

### MEDIUM-1 — Remembered routing is erased the moment the remembered card comes back (lane U)
- `src/app/MainComponent.cpp:1405-1434` (`checkRestoredDevice`): saved card X missing → JUCE opens Y → `deviceMismatch_ = true`, every slot disabled through `changeSlotConfig`, memory kept on disk (Q12).
- `:655-668` (`onAfterRestart`): clears `deviceMismatch_` when the device name differs from `mismatchDeviceName_`, then `saveSettings()`.
- `:1474-1509` (`saveSettings`): with the flag clear, the all-off routing is written over the file.
- Scenario: operator plugs X back in and selects it in the Geek DevicePanel. Only the wizard-cancel branch (`:999-1035`) re-applies `settings_.slots`. Result: card returns, routing gone, Dumb lamp says "Cần cài đặt".
- Fix: in `onAfterRestart`, when the open device matches `settings_.device` and `settings_.hasRouting`, re-apply `settings_.slots` via a helper shared with `onCancel`'s `onRemembered` branch, then clear the flag and save. For a different card Z, save with `hasRouting` untouched. Test: mismatch → restart onto X → slot 0 enabled and file unchanged.
- agent-safe: yes · DSP path: no

### MEDIUM-2 — Detector analysis window is never flushed on a sample-rate change
- `src/dsp/Detector.cpp:42-56` `Detector::reset()` has zero callers; `NotchController::setSampleRate` (`NotchController.cpp:1579-1600`) never calls `l.detector.reset()`.
- Scenario: 44.1→48 kHz restart. `history_` still holds 2048 old-rate samples; the first 3 hops are mislabelled (bin→Hz off by 8.8 % for ~32 ms). With AGGRESSIVE (`persistenceBlocks = 1`) one block can place a notch on a frequency that never rang.
- Fix: `l.detector.reset()` in the `setSampleRate` loop (thread stopped). Test: 1 kHz tone at 44.1 k, `setSampleRate(48000)`, first spectrum must not carry 1078 Hz energy. Expected level change: 0 dB steady state.
- agent-safe: yes · DSP path: detection, not output — but a wrong notch is audible

### MEDIUM-3 — `changeSlotConfig` still restarts everything, and lane U now calls it in loops
- `src/app/MainComponent.cpp:1701-1722`: one slot edit → `onBeforeRestart` (`:574-630`) joins 8 detectors and aborts the soundcheck; `onAfterRestart` (`:631-669`) resets all 8 and calls `saveSettings()` (synchronous file I/O) every time.
- Lane U loops it: `checkRestoredDevice` `:1427-1434`, `validateRestoredSlots` `:1451-1461`, `applyWizardResult` `:1527-1536`, `onCancel` `:990-997`, `:1021-1025`. Worst case at startup on a mismatch: 8 stop/start cycles, 64 joins, 8 file writes before the first frame.
- Fix: stop/apply/start only the edited slot's controller; save once per batch.
- agent-safe: no (A-R4 "reset every slot" is an owner ruling — owner card HW-174.Q3) · DSP path: no

### LOW
- **LOW-1** Wizard input meter dead for inputs 9+: `AudioEngine.h:175` `kInputMeterChannels = 8`; `takeInputPeak` `AudioEngine.cpp:290-294`; wizard offers all inputs (`SetupWizardModel.cpp:7-8`, `SetupWizard.cpp:250-255`). Fix: raise to 16. agent-safe yes · callback touched, read-only.
- **LOW-2** `startAudio` calls `setWidth`/`setSampleRate` on already-running controllers (`MainComponent.cpp:1313-1326`; contract `NotchController.h:210-216`). Benign today, TSan would flag. Fix: skip running controllers or reorder. agent-safe yes.
- **LOW-3** Model not re-validated against new Nyquist on rate change (`NotchController.cpp:1579-1600`): 96 k → 44.1 k with a 23 kHz notch shows Active in the GUI while the chain is Idle. Fix: clear entries ≥ rate/2 with a new `RateChange` reason. agent-safe yes.
- **LOW-4** Room-memory merge uses exact `==` on Hz (`NotchController.cpp:392`); sub-bin estimates differ per episode so duplicates fill the 16-entry ring. Fix: compare bins (as lookup at `:449` does). agent-safe yes.
- **LOW-5** Driver-initiated stop joins eight threads inline with a 5 s floor inside `audioDeviceStopped()` (`MainComponent.cpp:684-699`), likely under JUCE's callback lock. agent-safe no (owner trade-off, pairs with MEDIUM-3).
- **LOW-6** Deferred `onAfterRestart` leaves taps unread for one message-loop turn (`:700-717`); 8192-sample rings may overflow → false splice-drop counts. Fix: clear taps / reset counters in `onAfterRestart`. agent-safe yes.
- **LOW-7** `setUiMode` calls a test accessor in production (`MainComponent.cpp:1556`). Give `SetupWizard` a public `cancel()`. agent-safe yes.
- **LOW-8** CI sanitizer job is ASan+UBSan only (`.github/workflows/build.yml:181-187`); add a TSan job for detector↔UI races. `tests/CMakeLists.txt:482-494` duplicates a block. agent-safe yes.
- **LOW-9** Lane L1 `--ask` sends session-log summaries to a cloud API (`tools/copilot/copilot.py:264-269`); offline by default. Policy call for the owner. agent-safe no.

## 3. Checked and clean

- Sub-bin (P1): `PeakinessAnalyzer.cpp:37-68` guards; every Hz→bin consumer uses `lround`; harmonic penalty unaffected; level change documented (`docs/KY-THUAT-CHONG-HU.md:213-218`).
- Persisted settings: `HostSettings.cpp:67-78` schema guard, quarantine, atomic write, all-or-nothing slots.
- Wizard generation/confirm (`MainComponent.cpp:920-947`).
- Driver-restart hooks idempotent, nulled in dtor (`:1071-1072`).
- `healState()` on the audio thread alloc-free, bounded.

## 4. Suggested order
1. MEDIUM-1 → 2. MEDIUM-2 → 3. LOW-1/3/4/2/6/7 → 4. MEDIUM-3 + LOW-5 to the owner as one question → 5. LOW-8 TSan job.

## 5. Fix round (2026-11-09, Opus fix-round session)

| Item | Status | Where |
|---|---|---|
| MEDIUM-1 remembered routing | fixed | PR #35 (`1e50597`), tests in `test_lane_u_wiring.cpp` |
| MEDIUM-2 detector flush on rate change | fixed | PR #35, `NotchController.cpp` `setSampleRate` → `l.detector.reset()`; `test_notchcontroller.cpp` |
| LOW-1 input meter 16 ch | fixed | PR #35, `AudioEngine.h` `kInputMeterChannels = 16`; `test_audioengine.cpp` |
| LOW-2 setWidth/setSampleRate on running controller | fixed | PR #35, `MainComponent::startWorkersIfEngineRunning` skips running controllers |
| LOW-3 above-Nyquist notches on rate change | fixed | PR #35, `ClearReason::RateChange`; `test_notchcontroller.cpp` |
| LOW-4 room-memory merge by bin | fixed | PR #35, `NotchController.cpp` `lround(hz / binWidth)` compare |
| LOW-6 tap backlog after deferred restart | fixed | PR #35 |
| LOW-7 public `SetupWizard::cancel()` | fixed | PR #35 |
| Item (a) detectors gated on `engine_.isRunning()` | fixed | PR #35 (`startWorkersIfEngineRunning`); idle CPU with no card ~9 % → ~3–4 % of a core |
| LOW-8 TSan CI job + duplicate block in `tests/CMakeLists.txt` | fixed | PR #41 (`linux-tsan`, nightly + dispatch); first run 833/833 green, no race reported (run 42589473992) |
| Default window larger than small screens (UI test) | fixed | PR #36 (`fc94dc9`) |
| UI-test #3 version string 1.3.0 | open — owner decision | 1.3.0 IS the newest packaged build; 1.4.0 needs `release-alpha.ps1 -Part minor` (Q11), card HW-188 |
| MEDIUM-3, LOW-5, LOW-9, item (b), UI-test #2/#4 | not touched — owner decisions (HW-174.Q3) / needs an ASIO driver |
