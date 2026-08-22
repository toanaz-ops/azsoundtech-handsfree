# Lane brief — GUI device & status (Tasks 16, 17, 18, 23)

**Base:** `main` @ `194a090`. **Branch:** `feat/lane-gui-device`.
Read `README.md` beside this file first — setup, ground rules, and the three
worktree traps.

## Why this lane exists now

Until 2026-08-22 these four tasks were blocked: `AudioEngine` exposed no device
enumeration, no sample-rate or buffer-size setters, no channel counts and no
error text. Commit `194a090` landed all eleven methods, so this lane is open.

The rest of the GUI (Tasks 19, 20, 22, 24 — spectrum, notch overlay, notch list,
clear controls) is **still blocked** on the bridge design. Do not start it. This
lane is the half that does not need the detector.

## ⚠ Read this before anything else: the app has no audio

`AudioEngine` is **never instantiated**. Verified:

```
grep -rn "AudioEngine" src/main.cpp src/app/MainComponent.*   →  no matches
```

Tasks 8 and 9 built the engine and its tap; nothing constructs one. The shipped
app is an 800×600 window that draws the text "AZ Soundtech Hands-free" and
processes no audio at all. `MainComponent` is 25 lines with no members.

**So this lane's first job is not Task 16. It is making the app have an
AudioEngine.** Every task below assumes one exists. Nobody has flagged this, and
it is not in the plan.

Keep the ownership shallow: `MainComponent` owns the `AudioEngine` by value.
Do **not** make it a singleton or a global — the bridge design
(`docs/superpowers/specs/2026-08-22-audio-detector-bridge-design.md` §6) has
`MainComponent` owning both the engine and the future `NotchController`, and a
singleton would have to be undone.

## The interface you are building against

All on `AudioEngine`, all message-thread, all safe with **no device open** —
which is the state you are in while populating combo boxes at startup.

```
juce::StringArray   getAvailableDeviceTypeNames();   // non-const
juce::StringArray   getAvailableDeviceNames();       // non-const, current type
juce::String        getCurrentDeviceType() const;    // ACTUAL, not desired
juce::Array<double> getAvailableSampleRates();       // empty if no device
juce::Array<int>    getAvailableBufferSizes();       // empty if no device
bool   setSampleRate (double);                       // false if refused
bool   setBufferSize (int);                          // false if refused
double getCurrentSampleRateHz() const;               // NUMERIC form
int    getNumInputChannels()  const;                 // 0 until audio flows
int    getNumOutputChannels() const;
juce::String getLastDeviceError() const;             // "" when healthy
```

Plus what already existed: `start()`, `stop()`, `isRunning()`,
`setAudioDeviceType()`, `setAudioDevice()`, `getCurrentDeviceName()`,
`getCurrentSampleRate()` (**the display string** `"48000 Hz"`),
`getCurrentBufferSize()`, `getCurrentLatency()` (seconds), `setMode()`,
`getMode()`.

**Two names, one concept, on purpose.** `getCurrentSampleRate()` returns
`"48000 Hz"` for display; `getCurrentSampleRateHz()` returns `48000.0` for combo
selection. Use the right one. A test pins that they never disagree.

## Task 16 — Device selector

Plan says *"populate with `AudioDeviceManager::getAvailableDeviceTypes()`, filter
to show ASIO devices only"*.

**Do not filter to ASIO only.** `.gitignore` excludes `external/asiosdk/` for
licensing reasons, CI builds without it, and this dev machine has no ASIO driver
— an ASIO-only combo would be empty on every machine the project currently
builds on, including yours. Show all registered types, and default the selection
to ASIO when it is present.

Reflect the **actual** type via `getCurrentDeviceType()`, not the requested one.
`AudioEngine::start()` documents that JUCE silently keeps the current type when
the requested one is not registered, so echoing the request shows the user
"ASIO" while WASAPI is running.

Changing device: `stop()` → `setAudioDeviceType()` / `setAudioDevice()` →
`start()`. Both setters apply on the *next* `start()`, by design.

## Task 17 — Sample rate & buffer controls

The plan gives this one line (*"similar ComboBox/Label patterns"*) and it is the
most substantial of the four.

Populate from `getAvailableSampleRates()` / `getAvailableBufferSizes()`; both are
empty with no device, so the combos must be repopulated after a successful
`start()`.

**Honour the `bool`.** `setSampleRate` / `setBufferSize` return `false` when the
hardware refuses. On `false`, revert the combo to the current value and surface
why — do not leave the user looking at a setting the device is not running. That
is the specific failure the audit called out when it asked for `bool` rather than
`void`.

A rate change restarts the device, which fires `audioDeviceAboutToStart()` and
retargets both notch chains. You get that for free; do not re-implement it.

## Task 18 — Status display

Spec §6.1: `● 48 kHz / 2 in / 2 out / Latency 5.3 ms`

- Rate: `getCurrentSampleRate()` (display form)
- Channels: `getNumInputChannels()` / `getNumOutputChannels()`
- Latency: `getCurrentLatency()` — **seconds**, multiply by 1000
- Indicator: `isRunning()`

**Channel counts read 0 until audio has actually flowed.** That is deliberate,
not a bug — they are published from the callback, because a device can advertise
channels the callback is not handed. Render 0 honestly (e.g. "—") rather than
guessing 2.

**Show `getLastDeviceError()` when it is non-empty.** This is why the method
exists. Before `194a090` a device failure darkened the indicator with the reason
available nowhere in the UI — a soundman mid-show got a dead app and no cause.

## Task 23 — Mode buttons (PARTIAL — read this)

`setMode()` / `getMode()` exist, so Auto and Bypass are straightforward.

**The Soundcheck countdown is blocked and you must not fake it.** The button is
specified as "Run Soundcheck (15s)" with a countdown. Owner decision **D-06**
ruled that timers freeze while the tap is dead — so a GUI-side `juce::Timer`
counting wall-clock would disagree with a detector that had frozen, and would
show a countdown that does not match what the app is doing.

The remaining time must come from the detector, which does not exist yet
(bridge design §4, `getSoundcheckSecondsRemaining()`).

**Do:** wire Auto and Bypass fully; add the Soundcheck button so it sets
`Mode::Soundcheck`. **Do not:** implement a countdown. Leave a comment naming
D-06 and the bridge design as the reason.

## You own

```
src/gui/*                    (new)
src/app/MainComponent.h/cpp  (currently a 25-line stub)
CMakeLists.txt               (one line: your new sources)
tests/test_*                 (new files for your components)
```

**Do not touch** `src/dsp/*`, `src/app/AudioEngine.*`, or anything under
`installer/`. If you find you need an `AudioEngine` method that does not exist,
**stop and report** rather than adding it — that is how the header becomes a
merge conflict for four lanes at once.

## Done means

- `cmake --build build --config Release` exits 0
- `ctest -C Release` — 75/75 still pass, plus yours
- The app **runs**, opens a device, and the status line shows real values
- A screenshot or a description of what you saw on screen. This is a GUI lane;
  "it compiles" is not evidence.

## Known-wrong things in the plan, already corrected

| Plan says | Reality |
|---|---|
| Task 16: filter to ASIO only | Empty on every current build machine. Show all types. |
| Task 22: `notchChain.getNotchInfo()` | Private, and Task 22 is not in this lane. |
| Task 24: `clearNotch(i)` from the GUI | Superseded by D-05 — must route through the detector. Not in this lane. |
