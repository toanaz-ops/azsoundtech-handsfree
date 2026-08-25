# AZ Soundtech Hands-free

Windows standalone audio application for ASIO feedback elimination in live sound environments.

## System Requirements

- Windows 10/11 64-bit
- Visual Studio 2019 or later
- CMake 3.22 or later
- ASIO-compatible audio interface

## ASIO SDK Setup

Due to licensing, ASIO SDK must be downloaded manually:

1. Download ASIO SDK 2.3+ from Steinberg: https://www.steinberg.net/asiosdk
2. Extract to `external/asiosdk/` (should contain `common/asio.h`)
3. Re-run CMake configuration

A missing SDK does **not** fail the configure — it warns and silently builds
without the ASIO device type. Check for `JUCE_ASIO` in the generated project if
the app comes up with no ASIO driver listed.

## Submodules

- `external/JUCE` — the framework.
- `external/melatonin_blur` — MIT, https://github.com/sudara/melatonin_blur.
  Figma-accurate CPU drop and inner shadows. JUCE ships no usable soft shadow
  (`DropShadowEffect` is a bilinear smear), and the console uses real gaussians
  for the status LED halo and for the glow around a just-fired notch.

## Build Instructions

1. Clone the repository with submodules:
```bash
git clone --recursive <repository-url>
```

2. Configure the project with CMake:
```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```

   The generator is **2026, not 2022**. Keep `-G`: the `cmake` on PATH may be a
   MinGW build that defaults to Ninja, which rejects `-A x64`.

3. Build the project:
```bash
cmake --build build --config Release
```

## Testing

Run tests using CTest:
```bash
cd build && ctest -C Release
```

## Reviewing a GUI change

`HandsFreeSnapshot` renders the console to PNG with no window and no screen
capture, using `juce::Component::createComponentSnapshot()`:

```bash
build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast
```

It writes `console-idle.png` and `console-live.png` into `shots/`. Drop
`--fast` when the notch age ramp has to be visible — the full run takes about
23 seconds, because a notch cools from sodium to ice over ~22 seconds of wall
time and there is no way to show that without spending it.

Prefer this over screen-capturing the running app: a capture picks up whatever
window drifts in front of it, the desktop's DPI scaling rescales the result,
and the running binary holds a lock on its own `.exe` so the next build cannot
link. See `.claude/skills/juce-component-snapshot/SKILL.md`.

## License

Proprietary software. Requires activation. Copyright (c) 2026 AZ Soundtech. All rights reserved.
