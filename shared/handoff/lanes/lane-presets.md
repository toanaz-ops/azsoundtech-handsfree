# Lane brief — Preset format (Task 25 format half, Task 26)

**Base:** `main` @ `194a090`. **Branch:** `feat/lane-presets`.
Read `README.md` beside this file first — setup, ground rules, worktree traps.

## Scope: the format, not the wiring

Task 25 is two jobs and only one of them is available:

| | Status |
|---|---|
| Serialise / deserialise a preset to JSON on disk | **This lane.** Depends on nothing. |
| Install a loaded preset's notches into the running engine | **Blocked.** Owner decision D-05 routes every notch command through the detector, which does not exist yet. |

Build `PresetManager` as a pure component over a plain data struct. It should
know nothing about `AudioEngine`, `NotchChain` or the detector. Someone will
hand it a list of notches and ask it to write a file; someone will ask it to read
one back. That is the whole job, and keeping it that way is what lets it be
finished today and tested without an audio device.

## Task 26 just became possible

`Speech.json` (Q=40, depth −18 dB) and `Music.json` (Q=25, depth −10 dB) were
**not representable** until this session. `Biquad::setNotchFilter(freq, Q,
sampleRate)` took no depth argument at all — it was the RBJ pure notch, an
infinite-depth null — so every notch was a full null whatever depth was asked
for. Commit `0eab329` added the four-argument form and both values now work.

Two consequences for you:

- **Depth is a NEGATIVE number of dB.** `-18.0`, not `18.0`. This matches
  `NotchChain::setNotch`, the plan's JSON sample, and spec §6.1's display.
- **A positive depth is refused by the filter**, because the peaking form is
  symmetric and a positive gain would *boost* the ringing frequency. Your
  validation must reject positive depths at load time with a clear message
  rather than passing them down to be silently dropped.

## The format

From plan Task 25:

```json
{
  "version": "1.0",
  "device": "Audient iD14 MK2",
  "sampleRate": 48000,
  "bufferSize": 64,
  "notches": [ { "index": 0, "freq": 482.0, "Q": 30.0, "depth": -12.0 } ]
}
```

Location: `%APPDATA%/AZSoundtech/HandsFree/presets/`.
Use `juce::JSON` and `juce::File::getSpecialLocation (userApplicationDataDirectory)`.
Do not hand-roll JSON and do not hard-code the path.

**`sampleRate` is numeric here.** `AudioEngine::getCurrentSampleRateHz()` (added
in `194a090`) gives you the number. `getCurrentSampleRate()` returns the display
string `"48000 Hz"` — that is the wrong one, and using it would put a formatted
string into the file.

### Decide these, and write down what you decided

The plan does not say, and a preset file outlives the version that wrote it:

- **`"version": "1.0"` — what happens on a mismatch?** Refuse, or accept and
  migrate? Whatever you pick, a file with a future version must not be loaded as
  though it were current.
- **Loading a preset saved at a different sample rate.** A 482 Hz notch is fine
  at any rate, but a notch above the current Nyquist is not — that is the defect
  that produced a measured peak of 4.11e18 before it was guarded (see
  `Biquad.h`). Owner decision **D-00** rules that such notches go Idle while
  keeping their stored parameters, rather than being clamped. **Follow D-00.**
- **A preset with more than 16 notches per channel**, or a duplicate `index`.
  `NotchChain::MAX_NOTCHES` is 16.
- **The preset stores a `device` name that is not present.** Load the notches
  anyway, or refuse? A soundman opening last week's preset on a different
  interface is a normal Tuesday, not an error.

### Validate on load, and be specific about it

A preset file is user-editable and is the easiest way to get a bad value into the
DSP. Reject and report, one message per problem: `freq <= 0`, `freq >= sampleRate/2`,
`Q <= 0`, `depth > 0`, `index` out of `[0, 15]`, missing keys, wrong types.

**Round-trip test it.** Write a preset, read it back, assert every field is
identical — including the sign of `depth`, which is the field most likely to be
quietly flipped by a well-meaning `abs()`.

**Test the malformed cases too.** Truncated file, empty file, valid JSON that is
not a preset, a `notches` array containing a string. A preset loader that has
only ever been tested on files it wrote itself has not been tested.

## You own

```
src/app/PresetManager.h/cpp     (new)
presets/Speech.json             (new — location your call, say what you chose)
presets/Music.json              (new)
tests/test_presetmanager.cpp    (new)
CMakeLists.txt                  (one line: your new sources)
```

**Do not touch** `src/dsp/*`, `src/app/AudioEngine.*`, `src/app/MainComponent.*`,
`src/gui/*`. The GUI lane is live in `MainComponent` right now.

## Done means

- `cmake --build build --config Release` exits 0
- `ctest -C Release` — 75/75 still pass, plus yours
- Round-trip test passes, sign of `depth` included
- Malformed-input tests cover at least: truncated, empty, wrong types, depth > 0,
  freq above Nyquist
- Both default presets exist and **load through your own loader** in a test —
  not merely written by hand and eyeballed
- Your report states what you decided for each of the four open questions above

## What to hand back

Name precisely what the wiring half of Task 25 will need from you: the struct a
caller passes in, and the call the future `NotchController` will make to apply a
loaded preset. Under D-05 that caller routes through the **detector**, never
directly into `AudioEngine` — so design the hand-off as data, not as an action.
