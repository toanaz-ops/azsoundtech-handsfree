# Lane report — Installer (Task 30)

**Brief:** [lane-installer.md](lane-installer.md) · **Branch:**
`claude_desk/lane-c-installer-b0efb2` · **Date:** 2026-08-22

Task 30 is **done and verified by execution**. Task 31 remains blocked and was
not started, as the brief instructed.

---

## What was built

```
installer/handsfree.nsi          the installer script (the deliverable)
installer/fetch-deps.ps1         hash-pinned fetch of vendor/vc_redist.x64.exe
installer/verify-installer.ps1   end-to-end install / launch / uninstall test
installer/README.md              how to build it, and why it is shaped this way
installer/.gitignore             keeps vendor/ and the built setup out of git
```

`CMakeLists.txt` was **not** touched, and neither were `src/`, `tests/` or
`.github/`.

## Evidence

### It compiles

```
> makensis "/DBUILD_DIR=D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\build" installer\handsfree.nsi
Command line defined: "BUILD_DIR=D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\build"
Processing script file: "installer\handsfree.nsi" (ACP)
Output: "...\installer\AZSoundtech-Handsfree-Setup-1.0.0.exe"
Install: 4 pages, 1 section, 689 instructions
Uninstall: 2 pages, 1 section, 67 instructions
Total size:                 26480110 / 33400070 bytes (79.2%)
exit=0
```

The `1.0.0` in that filename was not typed anywhere in the script — it was
read out of `project(HandsFree VERSION 1.0.0)` in `CMakeLists.txt` at compile
time.

### It installs, runs and uninstalls

`pwsh -File installer\verify-installer.ps1` — **20 passed, 0 failed, exit 0**:

```
INSTALL (silent)
  PASS  setup exits 0 (got 0)
  PASS  app exe installed to Program Files
  PASS  uninstaller written
  PASS  install dir is under Program Files (not VirtualStore)
  PASS  Apps & Features entry exists in the 64-bit registry view
  PASS    DisplayName    = 'AZ Soundtech Hands-free'
  PASS    DisplayVersion is set
  PASS    Publisher      = 'AZ Soundtech'
  PASS    UninstallString points at a real file
        (DisplayVersion = 1.0.0, EstimatedSize = 7496 KB)
  PASS  Start Menu shortcut exists
  PASS  Start Menu shortcut resolves to the installed exe
  PASS  Start Menu uninstall shortcut exists
  PASS  Desktop shortcut exists
LAUNCH
  PASS  installed app is still running after 6 s (imports resolved)
UNINSTALL (silent)
  PASS  app exe removed
  PASS  install directory removed
  PASS  Apps & Features entry removed
  PASS  vendor registry key removed
  PASS  Start Menu folder removed
  PASS  Desktop shortcut removed
```

### The new tests, and what breaks each

The lane adds **no C++ tests** — it adds no C++. `ctest` was not run and its
total is not offered as evidence here. `verify-installer.ps1` is the test, and
each assertion has a production change that fails it:

| Assertion | Production change that makes it fail |
|---|---|
| install dir is under Program Files | delete `RequestExecutionLevel admin` → install redirects to VirtualStore |
| Apps & Features entry in 64-bit view | delete `SetRegView 64` → key lands in `WOW6432Node`, invisible to Apps & Features |
| Start Menu shortcut resolves to the exe | unquote the `CreateShortcut` target → NSIS takes `AZ` as the whole path |
| app still running after 6 s | remove the `vc_redist` block **on a machine without the runtime** — see caveat below |
| app exe removed | remove the `Delete`/`RMDir` lines from the uninstall section |
| Apps & Features entry removed | remove `DeleteRegKey` → entry survives uninstall, points at nothing |
| setup exits 0 | any `!error` guard firing, e.g. a missing build or missing `vendor/` |

**Caveat, stated plainly:** the launch assertion is a *weaker* test on this
machine than it will be elsewhere. This machine already has VC++ 14.51, so the
installer correctly skipped the redistributable and the app would have launched
either way. The assertion proves the app runs after install; it does **not**
prove the bundled redistributable works, because the code path that installs it
never executed here. Verifying that needs a clean VM and has not been done.

## Corrections to the plan and the brief

**The brief was right about all five things it warned of.** Confirmed rather
than overturned: `makensis` absent, `signtool` absent, and all three errors in
the plan's NSIS path (wrong directory, wrong filename, unquoted spaces).

Four things the brief did not have:

1. **The MSVC redistributable IS required.** The brief said to determine this
   from the real binary. The import table shows `MSVCP140.dll`,
   `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll` — all from the VC++ 2015–2022
   redistributable, none of them part of Windows. (The `api-ms-win-crt-*`
   imports also present *are* part of Windows 10+ and need nothing.) So the
   answer to the brief's open question is **yes**.

2. **`Delete` fails silently, and that was a real defect.** If the app is
   running at uninstall time the exe delete fails, sets the error flag, and
   execution continues — so the uninstaller would remove the registry key and
   shortcuts and report success, leaving the binary in Program Files with the
   Apps & Features entry that would let the user retry **already gone**. There
   is no route back through the UI. Fixed with a retry loop plus
   `Delete /REBOOTOK`. Found by the test, which uninstalls immediately after
   killing the app — ruder than a human tester would be, which is the point.

3. **NSIS resolves relative paths against the script's directory, not the
   shell's cwd.** Verified with a probe script: `makensis` run from the repo
   root still wrote `OutFile` into `installer\`. Wrapping paths in
   `${__FILEDIR__}` — which looks like the careful thing to do — **doubles the
   path** and breaks the build. Cost about ten minutes.

4. **NSIS is a 32-bit process.** Bare `HKLM` access is redirected into
   `WOW6432Node`, so the uninstall entry needs `SetRegView 64` or 64-bit Apps
   & Features never shows it while the installer still reports success.

## Owner decisions taken during this lane

Both of the brief's "ask before you decide these", plus one it did not
anticipate:

| Question | Decision |
|---|---|
| Bundle or check for an ASIO driver? | **Neither.** The installer does not mention ASIO. No Steinberg redistribution. |
| Write to `%APPDATA%/AZSoundtech/HandsFree/`? | Installer writes nothing there. Uninstaller **asks**, defaulting to *keep*, and never deletes during a silent uninstall — a silent uninstall is what an upgrade runs, and an upgrade must not eat a licence. |
| VC++ runtime (new question) | **Bundle** `vc_redist.x64.exe`, run silently, skip when ≥ 14.29 is present. |

The redistributable is **fetched, not committed**: 24.4 MB of incompressible
signed binary would sit in every clone forever. `fetch-deps.ps1` pins its
SHA-256 (`CC0FF0EB…B713B`, v14.44.35211.0) and fails loudly when Microsoft
rotates the file behind the `aka.ms` link.

## Not done, and why

- **Task 31 (code signing)** — blocked twice over, exactly as the brief said:
  no EV certificate purchased (a business decision, ~$300–500/yr) and
  `signtool` not installed. The exact commands, including the non-optional
  `/tr` timestamping and the app-before-installer signing order, are a
  commented block at the foot of `handsfree.nsi`. No self-signed placeholder:
  it would produce the identical SmartScreen warning Task 31 exists to remove.
- **No lane build.** This worktree has no `build/`, so `/DBUILD_DIR` pointed at
  the main checkout's real binary. Building here would have needed the shared
  JUCE junction, whose teardown the runbook flags as the most destructive step
  in the project — and the lane compiles no C++, so a lane build would have
  bought no information for that risk.
- **No CMake packaging target.** Would make CMake the single source of the
  build directory too, but `CMakeLists.txt` is the one file every lane touches.
  Worth adding once the lanes land.
- **Clean-VM test.** Not done. See the launch-assertion caveat above.

## Prerequisites the next person and CI both need

`makensis` **was not present** on this machine. Installed during this lane:

```bash
winget install --id NSIS.NSIS --exact
```

NSIS 3.12, landing at `C:\Program Files (x86)\NSIS\makensis.exe`, **not on
PATH**. `signtool` is still absent and was not installed.
