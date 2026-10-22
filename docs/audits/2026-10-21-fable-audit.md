<!-- Fable audit, 2026-10-21, read-only, against main @ caf8b66. Line numbers in this report are as of that commit and will drift; follow the symbols. -->

# Audit — PROJECT005-AZ-handsfree (read-only, HEAD of main worktree)

Scope read: CLAUDE.md, memory/MEMORY.md, README, docs/KY-THUAT-CHONG-HU.md (grep), all of `src/app/AudioEngine.*`, `src/dsp/{LockFreeRingBuffer,NotchChain,Biquad,Detector,CandidateScorer,PeakinessAnalyzer,SoundcheckSignal,LoopGainEstimator,ClockSource,NotchCommand}.*`, `src/app/{NotchController,SoundcheckController,SessionLogger,LicenseManager}.*`, targeted regions of `MainComponent.cpp` (ctor hooks, dtor, startAudio, requestMode, changeSlotConfig, loadPreset, timer), `DevicePanel.cpp`, `main.cpp`, `AzTheme.h`, `tests/CMakeLists.txt`, presets, TuningPanel constants, and the specific tests cited below. Skipped `.claude/worktrees`, `external/`, `build/`, `.opencode/`.

Audio-thread verdict first: `AudioEngine::audioDeviceIOCallbackWithContext` (D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\AudioEngine.cpp:602-1001) is clean — no heap, no locks, no logging, no I/O; only relaxed/acquire atomics, a 5 KB stack batch, SPSC ring writes, and libm calls inside `Biquad::designPeaking`. The three findings below that touch it are logic, not RT-safety.

---

### [SEV high] DETECTION threshold below 10 is dead for placement (scorer gates on the compile-time default)
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\dsp\CandidateScorer.cpp:52-58 — `if (candidate.peakiness <= PeakinessAnalyzer::kDefaultThreshold) return out;` and `pNorm = (peakiness / kDefaultThreshold - 1) / 9`.
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\dsp\PeakinessAnalyzer.cpp:137 uses the live `threshold_` (5..20); D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\NotchController.cpp:1331-1333 reinforce/deepen loop also uses the live threshold.
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\gui\TuningPanel.cpp:17 offers `{6, 8, 10, 12, 15}`; line 63 the shipped AGGRESSIVE response preset sets `thr = 8.0`.
- Why: a candidate with peakiness in [thr, 10] passes the analyzer, then scores exactly 0 → never confirms. Operators choosing 6/8 (or AGGRESSIVE) get no extra sensitivity, while reinforcement *does* use the lower threshold, so notches already placed deepen/hold on a looser rule than the one that places them. `docs/KY-THUAT-CHONG-HU.md:202` claims the threshold is runtime-tunable in [5,20].
- Fix: pass the live threshold into `scoreCandidateDetailed` (extra parameter from `la.analyzer.getThreshold()`), gate on it; decide separately whether `pNorm` should normalise on the live value (owner). Add a test: `setPeakinessThreshold(6)` + a tone with peakiness ~8 must place.
- Effort: S. Agent-safe: yes (gate); pNorm normalisation is owner taste.

### [SEV high] Notch commands queued while the device is down are discarded on the next device open → model says Active, chain is empty
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\AudioEngine.cpp:1087-1088 — `audioDeviceAboutToStart` does `queue.clear()` on all 8 command rings.
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\MainComponent.cpp:1021-1031 — `startAudio()` starts every detector thread even when `engine_.start()` failed.
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\NotchController.cpp:1582-1604 — `flushOutbox()` treats a successful `commands_.write()` as delivered.
- Why: ASIO not ready at launch (common) → preset load / manual notch / soundcheck APPLY → commands land in the ring, nothing drains (no callback) → operator picks the device in DevicePanel → `audioDeviceAboutToStart` wipes them. GUI/notch table/session log show protection that is not in the chain. This is exactly the model/chain divergence the bridge design forbids.
- Fix (either): (a) stop clearing `commandQueues_` in `audioDeviceAboutToStart` — Set commands carry Hz and `NotchChain::setNotch` re-validates against the new rate, so stale commands are safe; or (b) add `NotchController::resendModelLocked()` (one Set per active model notch) and call it from `onAfterRestart`. Add a test: write a Set into `getCommandQueue()`, call `audioDeviceAboutToStart(nullptr)`, run a callback, assert the chain is Active.
- Effort: S. Agent-safe: yes.

### [SEV high] Bypass copies instead of accumulates: two slots on one output lose a lane when BYPASS is hit, and the first lane's tap reads the other lane
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\AudioEngine.cpp:813-819 (`FloatVectorOperations::copy`) vs 826-856 (`out[n] += v`). Tap source is `out` (line 795), so lane A's detector gets lane B's samples.
- Only Auto is tested: D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\tests\test_audioengine.cpp:903-937 `TwoMonoSlotsSumIntoOneOutputChannel` sets `Mode::Auto`.
- Why: BYPASS is the emergency control; on a rig with shared outputs it changes the PA content/level (one mic disappears) — the opposite of "bypass".
- Fix: `FloatVectorOperations::add (out, in, numSamples)`. Expected level change: none for unshared routes; shared routes regain the overwritten lane(s) in Bypass (matches Auto today). Add the Bypass twin of the summing test.
- Effort: S. Agent-safe: yes, with the alpha-note callout CLAUDE.md requires for DSP-path changes.

### [SEV med] NaN self-heal strands a depth ramp (filter frozen at intermediate depth while model/GUI/preset read the target)
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\AudioEngine.cpp:849-853 calls `chain.reset()`; D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\dsp\NotchChain.cpp:105-116 and D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\dsp\Biquad.h:100-111 both document it as a known GAP. No test pins it.
- Why: one non-finite sample cancels every in-flight ramp in that chain; the filter runs a design nobody reports until the next retune or device restart.
- Fix: give `NotchChain` an allocation-free `healState()` that clears state then replays `setNotchFilter(stored freq/Q/depth)` for Active slots (same code as `setSampleRate` minus the rate write), and call that from the heal branch. State expected level change: heal lands on the target depth instead of an undefined intermediate one.
- Effort: S. Agent-safe: yes (unit test on `getFilterForTest`), alpha callout.

### [SEV med] Per-lane tap on a shared output carries the SUM, so both slots' detectors see each other's howl
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\AudioEngine.cpp:795 `tapSource[slot][lane] = out` where `out` is the shared, accumulated channel buffer; header comment (AudioEngine.h:161-163) promises "post-notch output of every lane".
- Why: a howl in slot 1's mic loop confirms in slot 0's detector too → two chains cut the same bin (double attenuation on one path, a wasted index on the other); INDEP/LINK reasoning assumes per-lane taps.
- Fix options: tap a pre-allocated per-lane scratch (16 × max-buffer floats) before summing, or document that the tap is the output channel. Which one is a product decision.
- Effort: M. Agent-safe: no (semantics).

### [SEV med] Editing one slot's routing resets ring-risk and wipes room memory on ALL eight slots
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\MainComponent.cpp:1109-1130 `changeSlotConfig` runs the full `onBeforeRestart/onAfterRestart` cycle (lines 573-647) without a device restart; `onAfterRestart` calls `setWidth()` on every slot, and D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\NotchController.cpp:117-126 makes `setWidth` unconditionally zero `blocksSinceReset` and clear room memory.
- Why: during a show, adding slot 4 flips every chip to N/A and forgets what every other room loop needed (A-R4 reset boundary was argued for device/SR changes, not for a sibling slot's routing edit).
- Fix: in `changeSlotConfig`, stop/setWidth/start only the changed controller (the rings are not cleared on that path, so §6.5 does not require joining the others).
- Effort: S. Agent-safe: no — A-R4 is an owner ruling; needs confirmation.

### [SEV med] Test gaps that let the above ship
- No placement test with threshold < 10 (only clamp/forwarding tests: D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\tests\test_notchcontroller.cpp:962-971, and a RunParams pass-through at test_gui_wiring.cpp:2552-2572).
- No Bypass + shared-output test (test_audioengine.cpp:903-937 is Auto only).
- No "commands survive `audioDeviceAboutToStart`" test (the restart tests at test_audioengine.cpp:198-215, 1739-1800 cover taps/mic ring only; test_gui_wiring.cpp:2279-2306 covers thread join only).
- No test for the NaN-heal ramp strand.
- Fix: one test each, listed under the findings. Effort: S each. Agent-safe: yes.

### [SEV low] `arm()` runs ~280 FFT-2048s on the message thread, and the same reference spectrum is recomputed for every target
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\SoundcheckController.cpp:291 → `enterTarget(0)` → line 814 `estimator_.pushReference(reference_)`; repeated per target from the lane M thread. The reference is identical every target (same `SoundcheckSignal` params).
- Fix: compute `referenceAccum_`/`referenceStream_.frames` once in `arm()` and have `LoopGainEstimator::reset()` restore them (or add `resetKeepingReference()`).
- Effort: S. Agent-safe: yes.

### [SEV low] Detector-thread allocations per block
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\NotchController.cpp:1465-1476 `std::vector<double> locked` built per lane per block (~190 Hz); lines 1584-1587 `flushOutbox` swaps `outbox_` with an empty vector so every later `push_back` reallocates.
- Fix: member scratch `std::array<double, kTotalSlots>` + count; swap with a reserved `pendingScratch_` member like `eventScratch_`.
- Effort: S. Agent-safe: yes.

### [SEV low] Eight detector threads poll at 200 Hz with no device
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\MainComponent.cpp:1026-1031 starts controllers unconditionally after `engine_.start()`; with no device they wake every 5 ms forever and are the producer side of the lost-command finding above.
- Fix: start only when `engine_.isRunning()`; the after-restart hook already starts them on a later successful open.
- Effort: S. Agent-safe: yes.

### [SEV low] 1 s `stopThread` timeouts can kill a thread holding `modelMutex_`
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\NotchController.cpp:42-46 (`stopThread(timeoutMs)`), called with 1000 ms from MainComponent.cpp:628, 828, 1878 and `SoundcheckController::stop(1000)` at SoundcheckController.cpp:350. JUCE force-kills on timeout; a kill inside a locked region leaves the mutex locked and the next GUI `setNotch`/`copySnapshot` deadlocks.
- Why it is unlikely: the loop step is 5 ms and the sink only takes a short logger mutex — but a slow disk inside `SessionLogger::log`'s JSON serialisation (3×1025 floats per Set) is on that path.
- Fix: raise to ≥5 s and assert on timeout, or move the event-sink flush out of the join window.
- Effort: S. Agent-safe: yes.

### [SEV low] Ramp-out request can get stuck pending until the next device restart
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\AudioEngine.cpp:866-917 — `scSampleIndex_` only advances and the "run ends itself" branch only fires when `scOut >= 0`; if the armed channel is out of range for this callback (channel count shrank without a restart), `requestSoundcheckRampOut()` (SoundcheckController.cpp:1083-1093) sets a flag no block clears, and `arm()` refuses `RampOutPending` (SoundcheckController.cpp:200-201) until `audioDeviceAboutToStart`.
- Fix: in the callback, when `scOutChannel >= 0 && scOut < 0`, release immediately (`scOutChannel_ = -1`, clear ramp state) — nothing is audible on an unmapped channel.
- Effort: S. Agent-safe: yes.

### [SEV low] `AudioEngine::stop()` is a no-op after `audioDeviceError`
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\AudioEngine.cpp:94-106 keys teardown on `isRunning_`, which `audioDeviceError` (1098-1103) already cleared, so the callback is never removed and `closeAudioDevice()` never runs; `DevicePanel::restartWith` (DevicePanel.cpp:104-107) then re-initialises with the callback still registered (JUCE dedups, so it works by accident).
- Fix: separate "callback registered" from "device healthy"; always remove/close in `stop()` when registered.
- Effort: S. Agent-safe: yes.

### [SEV low] Device is opened twice on every type switch (verify on hardware)
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\AudioEngine.cpp:59-67 — `setCurrentAudioDeviceType(type, true)` opens the type's default device, then `initialise(..., desiredDeviceName_)` opens the named one. On ASIO that is two driver inits per switch.
- Fix: pass `treatAsChosenDevice = false` when a device name will be supplied, then `initialise`. Needs a real ASIO driver to confirm there is no regression in device selection.
- Effort: S. Agent-safe: no (hardware).

### [SEV low] Dead and duplicated code
- `CandidateScorer::kRiseThreshold` (D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\dsp\CandidateScorer.h:45) is unused; the literal `0.5f` at CandidateScorer.cpp:107 is the same number.
- `NotchController::setSampleRate` has no production caller (NotchController.cpp:1573-1575 admits it); the device path goes through `setWidth`.
- `gui::StatusBar` and `gui::ModeBar` are hidden but constructed and updated every timer tick (MainComponent.h:498-502, MainComponent.cpp:2193, 2244).
- `SoundcheckController::State::{Preflight,Confirm,Arm}` are never entered (SoundcheckController.h:124-131).
- `AudioEngine::getCommandQueue(int)` inlines the clamp that `clampSlot` already provides (AudioEngine.cpp:478 vs 297); `flatSlot` (SoundcheckController.cpp:1241) duplicates `NotchController::slotOf`.
- `LicenseManager`/`LicenseViewModel` are fully built, tested and unwired (README:102-104) — fine as a decision, listed for completeness.
- Fix: delete `kRiseThreshold` or use it; remove the two hidden panels once lane tests are migrated; keep the rest unless the owner wants the surface smaller.
- Effort: S. Agent-safe: yes for the constant/clamp; no for removing the hidden panels (owner said "do not delete yet").

### [SEV low] README version drift
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\README.md:10-11 says "Current version: 1.1.3 (1.2.0 in alpha gate)"; D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\CMakeLists.txt:3 is `1.3.0` and `docs/release-notes/1.3.0-alpha.md` exists.
- Fix: drop the number from README (CLAUDE.md already says the version lives only in `project()`).
- Effort: S. Agent-safe: yes.

### [SEV low] Stale line-number and geometry references in comments
- D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\dsp\Biquad.h:100 "AudioEngine.cpp:617" → actual 851; D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\dsp\SoundcheckSignal.h:50 "AudioEngine.cpp:642" → ~935; D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\SoundcheckController.h:419-420 "MainComponent.cpp:802-803" → 1104; D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\app\SoundcheckController.cpp:129 "AudioEngine.cpp:546-548" → 742-744; D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\src\dsp\PeakinessAnalyzer.cpp:104 still says "5 * 48000/1024 = 234.375 Hz" (FFT is 2048 → ~117 Hz, as the header itself states).
- Fix: reference symbols, not line numbers.
- Effort: S. Agent-safe: yes.

---

Not found (looked for, clean): heap/lock/IO on the audio thread; memory or handle leaks (all threads joined in declaration order, `SessionLogger` stream is a `unique_ptr`, `juce::var` objects are refcounted); SPSC ring correctness (indices monotonic, acquire/release correct, `clear()` precondition honoured by the restart hooks); preset writes are atomic via `File::replaceWithText`.

## Executive summary
1. The real-time path is clean; the three high findings are logic bugs around it: a dead sensitivity knob (scorer gates on the compile-time threshold, shipping AGGRESSIVE preset affected), notch commands silently discarded when a device opens after the fact, and BYPASS overwriting instead of summing on shared outputs.
2. All three are small fixes (S) with obvious unit tests and are agent-safe; the Bypass and NaN-heal fixes touch the DSP path and need the CLAUDE.md "expected level change" note in the alpha release note.
3. Two medium items need an owner call rather than code: what a per-lane tap means on a shared output channel, and whether a routing-table edit should reset every slot's room memory / ring-risk readout.
4. Perf is not a problem today; the only hot-ish spots are detector-thread allocations and a redundant per-target reference FFT in the soundcheck — both S.
5. Doc/code drift is minor (README version, stale line refs, the KY-THUAT claim that threshold is tunable in [5,20] which is only half true until finding 1 is fixed); the memory/ index and CLAUDE.md are accurate.
