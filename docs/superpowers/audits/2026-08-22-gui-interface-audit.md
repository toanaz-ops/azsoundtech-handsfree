# GUI interface audit — Tasks 16–26 against the current headers

**Lane C of the parallel execution plan.** Read-only. Written 2026-08-22 against
`main` @ `51cf65e`.

**Purpose.** The parallel execution plan §2 argues the full public surface of
`AudioEngine` and `Detector` should land in **one commit** before the GUI lane
starts, so GUI / licensing / installer can run concurrently without widening the
same headers and colliding. This document produces that surface: every accessor
each task actually needs, the plan line that requires it, and whether it exists
today.

> **Tóm tắt cho chủ dự án.** Đã đối chiếu Task 16–26 với header hiện tại.
> Kết quả: **21 phương thức còn thiếu**, trong đó 3 cái là lỗi thiết kế thật sự
> chứ không chỉ là "chưa viết":
>
> 1. `getCurrentSampleRate()` trả về **chuỗi hiển thị** `"48000 Hz"` chứ không
>    phải số — ComboBox của Task 17 không dùng được để chọn đúng mục.
> 2. Không có setter nào cho sample rate và buffer size — Task 17 **không thể
>    làm được** với interface hiện tại.
> 3. Không có cách nào đọc trạng thái notch để hiển thị — Task 20, 22, 25 đều
>    cần, và cả ba đang bị chặn.
>
> Ngoài ra `depthDB` vẫn chưa được áp dụng ở đâu cả, nên Task 26 (preset
> −18 dB / −10 dB) chưa biểu diễn được. Xem mục "Blockers" cuối tài liệu.

---

## Method

For each task the plan defines, I read the task text, then grepped the current
headers for the symbol it names or implies. Claims below are stated as
**verified** only where I ran the check; where the plan's own text is the source
I say so. Nothing here is taken from a previous session's report — this project
has a documented history of green claims that were false.

Baseline read: `src/app/AudioEngine.h` (131 lines), `src/dsp/Detector.h` (139),
`src/dsp/NotchChain.h` (74), `src/dsp/NotchCommand.h` (18).

---

## 1. What exists today

`AudioEngine` public surface, verified by reading lines 39–105 of the header:

| Method | Returns | Thread |
|---|---|---|
| `start()` / `stop()` / `isRunning()` | `void` / `void` / `bool` | message |
| `setAudioDeviceType(const String&)` | `void` | message, applied on next `start()` |
| `setAudioDevice(const String&)` | `void` | message, applied on next `start()` |
| `getCurrentDeviceName()` | `juce::String` | message |
| `getCurrentSampleRate()` | **`juce::String`** ⚠ | message |
| `getCurrentBufferSize()` | `int` | message |
| `getCurrentLatency()` | `double` (seconds) | message |
| `setMode(Mode)` / `getMode()` | `void` / `Mode` | any (atomic) |
| `getTapBuffer()` | `LockFreeRingBuffer<float>&` | detector only, read-side |
| `getTapDropCount()` | `std::uint64_t` | any (atomic) |

`Detector` public surface: `setSampleRate` / `getSampleRate`,
`processLatestBlock(tap)`, `reset()`, and one documented test-only accessor.

That is the whole surface. Everything below is missing.

---

## 2. Task-by-task gaps

### Task 16 — Device Selector

Plan: *"juce::ComboBox populated with `AudioDeviceManager::getAvailableDeviceTypes()`,
filter to show ASIO devices only. On change, call `audioEngine.stop()`,
reconfigure, `start()`."*

| Needs | Status |
|---|---|
| Enumerate device **types** | ❌ `deviceManager_` is private (`AudioEngine.h:107`) |
| Enumerate device **names** for the current type | ❌ same |
| Read back the **current type** to preselect the combo | ❌ only `desiredDeviceType_`, private, and it is the *desired* not the *actual* |
| `stop()` / `setAudioDeviceType()` / `setAudioDevice()` / `start()` | ✅ all exist |

**Note on the plan's approach.** The plan says the GUI should call
`AudioDeviceManager::getAvailableDeviceTypes()`. Exposing `deviceManager_`
directly would hand the GUI the ability to call `addAudioCallback`,
`closeAudioDevice` and `initialise` behind `AudioEngine`'s back, defeating the
lifecycle `start()`/`stop()` maintain. Two narrow accessors returning
`juce::StringArray` are the smaller surface.

**A real distinction the plan misses.** `desiredDeviceType_` and the *actual*
open device type are not the same value. `AudioEngine::start()` comments say
JUCE "silently keeps the current type" if the requested type is not registered —
which is exactly what happens when ASIO is requested on a machine with no ASIO
driver. A GUI that reads back the desired value shows the user ASIO selected
while WASAPI is running. The getter must report the **actual** device type.

### Task 17 — Sample Rate & Buffer Controls

Plan: *"Similar ComboBox/Label patterns."* — one line for the task that is most
blocked.

> **Update 2026-08-23:** `setSampleRate(double)` and `setBufferSize(int)` have
> landed since this audit was written (commit `194a090` plus the device lane),
> and the ⚠ display-vs-numeric split below was resolved as
> `getCurrentSampleRateHz()`. Rows 1–3 of this table are stale; the audit text
> is kept verbatim as a record.

| Needs | Status |
|---|---|
| `setSampleRate(double)` | ❌ **does not exist** — verified by grep over `src/app/` |
| `setBufferSize(int)` | ❌ **does not exist** |
| Available sample rates for the open device | ❌ |
| Available buffer sizes for the open device | ❌ |
| Current sample rate **as a number** | ❌ ⚠ see below |
| Current buffer size as a number | ✅ `getCurrentBufferSize()` |

⚠ **`getCurrentSampleRate()` returns a display string, not a value.** The
implementation is `juce::String(currentSampleRate_.load(...)) + " Hz"`
(`AudioEngine.cpp:97-100`). A `ComboBox` populated with `44100, 48000, 96000`
cannot select the matching item from `"48000 Hz"` without parsing its own
formatting back out. Task 18's status line wants the formatted string; Task 17's
combo wants the double. **Both are needed and they are different methods** — the
current name is claimed by the display form, so the numeric one needs a distinct
name.

This is the single clearest example of why the plan wants the interface landed
before the GUI lane: whichever GUI task hit this first would have renamed or
re-typed an existing public method, and every other lane would have rebased onto
it.

### Task 18 — Status Display

Spec §6.1 shows: `● 48 kHz / 2 in / 2 out / Latency 5.3 ms`

| Needs | Status |
|---|---|
| Sample rate, formatted | ✅ `getCurrentSampleRate()` |
| Latency | ✅ `getCurrentLatency()` |
| Running indicator (`●`) | ✅ `isRunning()` |
| **Input channel count** | ❌ |
| **Output channel count** | ❌ |
| Last device error, to explain a red `●` | ❌ — `audioDeviceError()` logs the message and clears `isRunning_`, then **discards the string** (`AudioEngine.cpp:293-299`) |

The error case matters more than it looks. Today a device failure makes the
indicator go dark with no explanation anywhere in the UI; the reason exists only
in the JUCE log. A soundman mid-show gets a dead app and no cause.

### Task 19 — Spectrum Analyzer

Plan: *"Subscribe to detector FFT output (via thread-safe queue or atomic pointer
swap) … update at 60 FPS."*

| Needs | Status |
|---|---|
| A spectrum snapshot safe to read from the message thread | ❌ **`Spectrum::magnitudes` is a borrowed pointer**, documented invalid after the next `processLatestBlock()` (`Detector.h:70-71`) |
| The sample rate the magnitudes belong to | ✅ carried in `Spectrum::sampleRate` — already designed against a torn pair |

**This is Lane B's item 4, not a Lane C gap.** The audit records it here because
Task 19 cannot start until the bridge design settles the publication mechanism.
The plan's own wording ("queue **or** atomic pointer swap") is the un-made
decision.

### Task 20 — Notch Overlay Markers

Plan draws a marker per active notch, with depth as marker height. Spec §6.1 adds:
*"Notch mới detect (vài giây gần đây) hiển thị khác màu."*

| Needs | Status |
|---|---|
| Per-channel notch state readable off the audio thread | ❌ `notchChains_` is private; `NotchChain.h:13-16` says all methods are audio-thread-only |
| `frequency`, `Q`, `state` | ⚠ exist in `NotchChain::NotchInfo` but are unreachable |
| `depthDB` for marker height | ⚠ stored in `NotchInfo`, **applied by nothing** — see Blockers |
| **Age of the notch** ("recently detected → different colour") | ❌ **`NotchInfo` has no timestamp field at all** |

### Task 21 — Real-time Update Optimization

*"Ensure no allocation in paint path. Pre-allocate Path objects, swap buffers."*
No interface requirement. Constrains Task 19's snapshot to be **copy-into-caller-
owned-storage** rather than allocate-and-return, so the paint path stays
allocation-free.

### Task 22 — Notch List Table

Columns: `# | Freq | Depth | Q | Status | Locked Time`.
Plan says *"Populate from `notchChain.getNotchInfo()`"* — which is private.

Same gaps as Task 20, plus **Locked Time is a column with no backing field.**
`NotchInfo` is `{frequency, Q, depthDB, state}`. Spec §5.1 lists
`locked_at (time)` as a per-notch parameter; it was never implemented.

### Task 23 — Mode Buttons

| Needs | Status |
|---|---|
| `setMode` / `getMode` | ✅ |
| Soundcheck countdown for *"Run Soundcheck (15s)"* | ❌ nothing tracks the remaining time |

Whether the countdown lives in the GUI or the detector is a design question, not
an audit finding — it depends on D-06 (timers freeze while the tap is dead),
which means the GUI **cannot** own it: a GUI-side `Timer` counts wall-clock and
would disagree with a detector that froze. **The detector must expose the
remaining time.**

### Task 24 — Clear Controls

Plan: *"'Clear All' button → calls `notchChain.clearNotch(i)` for all i."*

⚠ **This plan text is superseded by owner decision D-05.** The detector owns the
notch model and is the sole command producer; a GUI that calls `clearNotch`
directly would leave the detector's model claiming notches that no longer exist,
and its auto-release timers pointing at nothing. Clear must be a **request to
the detector**, not a direct call.

| Needs | Status |
|---|---|
| `requestClearAll()` | ❌ |
| `requestClearNotch(channel, index)` | ❌ |

### Task 25 — Preset Save/Load

Save needs to read notch state (same gap as Task 20/22) plus device name, sample
rate **as a number**, and buffer size. Load needs to install notches — via the
detector, per D-05.

The JSON format in the plan stores `sampleRate: 48000` as a number, confirming
independently that the numeric getter is required.

### Task 26 — Default Presets

`Speech.json` Q=40 depth=−18 dB, `Music.json` Q=25 depth=−10 dB.
**Blocked** — see Blockers.

---

## 3. The consolidated interface

Grouped by owner. Threading contract is part of the signature's meaning, so it is
stated for every method.

### `AudioEngine` — device enumeration and control (message thread)

```
juce::StringArray getAvailableDeviceTypeNames() const;
juce::StringArray getAvailableDeviceNames() const;      // for the current type
juce::String      getCurrentDeviceType() const;         // ACTUAL, not desired

juce::Array<double> getAvailableSampleRates() const;
juce::Array<int>    getAvailableBufferSizes() const;

bool setSampleRate (double newRate);                    // false if rejected
bool setBufferSize (int newSize);                       // false if rejected

double getCurrentSampleRateHz() const;                  // numeric companion to
                                                        // getCurrentSampleRate()
int    getNumInputChannels() const;
int    getNumOutputChannels() const;
juce::String getLastDeviceError() const;                // "" when healthy
```

Eleven methods. `setSampleRate` / `setBufferSize` return `bool` rather than
`void` because a device can refuse a rate, and a GUI that cannot tell success
from silent refusal will show the user a value the hardware is not running.

### Notch state read-back (audio thread → any reader)

Needed by Tasks 20, 22 and 25. The shape is a Lane B decision; the **content**
is a Lane C finding, and it is larger than `NotchChain::NotchInfo`:

```
frequency   Hz          — exists in NotchInfo
Q                       — exists in NotchInfo
depthDB                 — exists in NotchInfo, applied by nothing (Blocker 1)
state       Idle|Active — exists in NotchInfo
lockedAtMs              — DOES NOT EXIST; required by Task 22's "Locked Time"
                          column and spec §5.1's locked_at, and by spec §6.1's
                          "recently detected shows in a different colour"
```

Since `lockedAtMs` cannot live in `NotchInfo` (an audio-thread struct that must
not carry a clock), and D-05 puts the authoritative model in the detector, the
snapshot the GUI reads should come from the **detector**, not from `NotchChain`.
This resolves Task 20/22's blocker without any audio-thread read-back at all.

### Detector / controller — command requests (message thread → detector)

```
void requestClearAll();
void requestClearNotch (int channel, int index);
void requestSetNotch   (int channel, double freq, double Q, double depthDB);
void requestLoadPreset (<preset notch list>);            // D-05: adopted
double getSoundcheckSecondsRemaining() const;            // Task 23 countdown
```

### Detector — spectrum publication (detector thread → message thread)

Shape is Lane B's decision. Constraint from Task 21: it must copy into
caller-owned storage, not allocate.

---

## 4. Corrections to the plan found by this audit

1. **Task 24's implementation line is wrong** given D-05 — `clearNotch` must not
   be called from the GUI. Recorded above.
2. **Task 30's NSIS path is wrong**, both directory and filename. Verified this
   session by locating the real artefact:
   `build/HandsFree_artefacts/Release/AZ Soundtech Hands-free.exe`
   The plan says `..\build\Release\HandsFree.exe`. Both the folder and the stem
   differ, and the real name contains **spaces**, so the NSIS `File` line needs
   quoting the plan's version does not have.
3. **Task 22 lists a column with no backing data.** "Locked Time" has no field
   anywhere in the codebase.
4. **Task 17 is one line in the plan** (*"Similar ComboBox/Label patterns"*) and
   is the most blocked task in the GUI lane — it needs two setters and two
   enumerators that do not exist, plus a numeric getter that collides with an
   existing name.

---

## 5. Blockers this audit cannot resolve

**Blocker 1 — `depthDB` is stored and applied by nothing.**
Every notch is therefore a full-depth null. Spec §5.1 requires 6–24 dB. This
blocks Task 26's presets outright (−18 dB and −10 dB are not representable),
makes Task 22's Depth column display a number with no effect, and makes Task 20's
depth-as-marker-height meaningless. It is a DSP fix, independent of the bridge,
and should land before the GUI displays the value.

**Blocker 2 — spectrum publication shape** (Task 19) is a Lane B decision.

**Blocker 3 — notch snapshot shape** (Tasks 20, 22, 25) is a Lane B decision,
though D-05 already determines *where the data lives* (the detector).

---

## 6. Recommendation

Land the eleven `AudioEngine` methods of §3 as one commit **now** — they depend
on nothing in Lane B, and they are what Tasks 16, 17 and 18 are waiting on. That
splits the GUI lane in two: device/status work (16, 17, 18, 23) can start
immediately, while visualisation and notch-list work (19, 20, 22, 24, 25) waits
for the bridge design.

The plan assumed one interface commit unblocks the whole GUI lane. The audit
finds it unblocks a bit more than half of it, sooner.
