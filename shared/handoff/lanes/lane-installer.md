# Lane brief — Installer (Task 30)

**Base:** `main` @ `194a090`. **Branch:** `feat/lane-installer`.
Read `README.md` beside this file first — setup, ground rules, worktree traps.

Smallest lane, entirely in a new directory. Task 31 (code signing) is in the
same plan phase but is **hard-blocked** — see the bottom.

## ⚠ The plan's NSIS script is wrong in three ways

Plan Task 30 contains:

```nsis
File "..\build\Release\HandsFree.exe"
```

Every part of that path is wrong. Verified against a real build this session:

```
build/HandsFree_artefacts/Release/AZ Soundtech Hands-free.exe
```

1. **Wrong directory.** JUCE emits into `HandsFree_artefacts/Release/`, not
   `Release/`. That is what `juce_add_gui_app` does; it is not configurable
   without changing the CMake target.
2. **Wrong filename.** The product name is `AZ Soundtech Hands-free`, not
   `HandsFree`. `HandsFree` is the CMake *target* name, which is a different
   thing.
3. **The real name contains spaces**, and the plan's `File` and `CreateShortcut`
   lines do not quote it. NSIS will parse `AZ` as the argument and choke on the
   rest.

The same three errors apply to the plan's `CreateShortcut` line, which points at
`$INSTDIR\HandsFree.exe`.

## `makensis` is not installed

```
command -v makensis   →  NOT FOUND
```

You need NSIS before you can compile or test anything. Install it (winget:
`NSIS.NSIS`) and **say in your report that it was not present**, so the next
person and CI both know it is a prerequisite rather than an assumption.

**A script that has never been compiled is not done.** This project has a
documented history of "verified" claims that were never executed. Run
`makensis` and quote the output.

## What to build

`installer/handsfree.nsi`, producing
`AZSoundtech-Handsfree-Setup-1.0.0.exe`, installing to
`$PROGRAMFILES64\AZSoundtech\HandsFree`.

Beyond the plan's skeleton, all of which it omits:

- **An uninstaller.** The plan has an `Install` section and no `Uninstall`. An
  installer with no uninstaller fails Windows conventions and leaves users no
  way out. Register it under
  `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\...` so it appears in
  Apps & Features.
- **`RequestExecutionLevel admin`.** Writing to `$PROGRAMFILES64` needs
  elevation. Without this the install silently lands in the VirtualStore on some
  configurations and the shortcut points at nothing.
- **The JUCE runtime dependency.** Check what the built exe actually links.
  If it needs the MSVC redistributable, the installer must handle it — a
  first-run `VCRUNTIME140.dll` error is indistinguishable from a broken product.
  Determine this by inspecting the real binary, not by assuming.
- **A version string in one place**, so `PRODUCT_VERSION`, the output filename
  and the uninstall registry entry cannot drift apart.

## Ask before you decide these

- Whether the installer should bundle the ASIO driver or check for one. This app
  defaults to the ASIO device type and `.gitignore` excludes `external/asiosdk/`
  for licensing reasons. Redistributing Steinberg's SDK has licence conditions —
  **do not decide this yourself.**
- Whether the installer writes anything to
  `%APPDATA%/AZSoundtech/HandsFree/` (presets, licence). Another lane owns those
  paths; coordinate rather than guess.

## You own

```
installer/*        (new directory)
CMakeLists.txt     (only if you add a packaging target — one line)
```

**Do not touch** `src/`, `tests/`, or `.github/`. If the exe path changes you
need `CMakeLists.txt` looked at, **report it** rather than editing the target.

## Task 31 — Code signing: blocked, do not start

Two independent blockers, neither of which is code:

1. **No certificate.** Task 31 needs an EV code-signing certificate, roughly
   $300–500/year. Nobody has bought one. This is a business decision.
2. **`signtool` is not installed** either (it ships with the Windows SDK).

Write the signing step as a **commented, unexecuted** block in the script, with
the exact command the owner will need, and stop there. Do not add a fake
signature, a self-signed certificate, or a placeholder path that looks real —
a self-signed binary produces the same SmartScreen warning Task 31 exists to
remove, so it would look done while achieving nothing.

## Done means

- `makensis installer/handsfree.nsi` **compiles**, output quoted in your report
- The produced installer **installs and uninstalls cleanly** on this machine
- The installed app **launches** (note: it currently opens a window and processes
  no audio — `AudioEngine` is not yet instantiated; that is the GUI lane's job,
  not a bug in your installer)
- Your report names: that `makensis` was missing, the corrected artefact path,
  and whether a runtime redistributable is required
