# Installer — AZ Soundtech Hands-free (plan Task 30)

Builds `AZSoundtech-Handsfree-Setup-<version>.exe`, which installs to
`%PROGRAMFILES%\AZSoundtech\HandsFree` for all users and registers a proper
uninstaller in Apps & Features.

## Build it

```bash
pwsh -File installer\fetch-deps.ps1
```

```bash
makensis installer\handsfree.nsi
```

`makensis` is **not** part of this repo's toolchain and was not installed on the
build machine — install NSIS first:

```bash
winget install --id NSIS.NSIS --exact
```

It lands at `C:\Program Files (x86)\NSIS\makensis.exe` and is **not added to
PATH**, so call it by full path or add it yourself.

If the app is built somewhere other than `<repo>\build`, point the script at it:

```bash
makensis "/DBUILD_DIR=D:\path\to\your\build" installer\handsfree.nsi
```

## Verify it

```bash
pwsh -File installer\verify-installer.ps1
```

Really installs, really launches the app, really uninstalls, and asserts 20
things about the result. Needs elevation and re-launches itself once via UAC.
Exit code is the number of failed assertions, so CI can gate on it.

## How this fits together

| File | Role |
|---|---|
| `handsfree.nsi` | The installer script. The deliverable. |
| `fetch-deps.ps1` | Downloads `vendor\vc_redist.x64.exe`, hash pinned. |
| `verify-installer.ps1` | End-to-end install/launch/uninstall test. |
| `vendor/` | Fetched, **gitignored**. Never committed. |

## Decisions worth knowing before you change this

### The version is read out of CMakeLists.txt

`handsfree.nsi` does not contain a version number. It `!searchparse`s the
`project(HandsFree VERSION x.y.z)` line at compile time, and derives
`PRODUCT_VERSION`, the output filename and the Apps & Features `DisplayVersion`
from that one value. CMakeLists already carries two copies of "1.0.0"; a third
here would be one more thing to forget.

Reword that `project()` line and the installer **stops compiling**. That is the
intended behaviour — an installer stamped with a guessed version is worse than
one that refuses to build.

### The Visual C++ runtime is bundled, and it is genuinely required

Read out of the built binary's import table, not assumed:

```
MSVCP140.dll   VCRUNTIME140.dll   VCRUNTIME140_1.dll
```

Those three ship in the VC++ 2015–2022 redistributable and are **not** part of
Windows. (The `api-ms-win-crt-*.dll` imports also present *are* part of
Windows 10+ and need nothing.) Without the redistributable, first launch on a
clean machine dies with a `VCRUNTIME140.dll` dialog, which a user cannot tell
apart from a product that simply does not work.

The installer checks
`HKLM\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\X64` and skips the
~10 s redistributable run when ≥ 14.29 is already present, which is the common
case.

`vc_redist.x64.exe` is 24.4 MB of incompressible signed binary, so it is
**fetched, not committed** — it would otherwise sit in every clone of this repo
forever, undeltable. `fetch-deps.ps1` pins its SHA-256. When Microsoft rotates
the file behind the `aka.ms` link the hash check fails loudly; re-verify, then
update the hash in one reviewable commit. Do not relax the check.

### Relative paths resolve against the *script*, not the shell

Verified with a probe: `makensis` invoked from the repo root still wrote
`OutFile` into `installer\`. So `..\CMakeLists.txt`, `..\build` and
`vendor\vc_redist.x64.exe` are all correct as written, and wrapping them in
`${__FILEDIR__}` **doubles the path** and breaks the build.

### NSIS is a 32-bit process

Every `HKLM` read and write is silently redirected into `WOW6432Node` unless
`SetRegView 64` is set first. Get that wrong and the app never appears in
64-bit Apps & Features while the installer still reports success.
`verify-installer.ps1` asserts the key in the 64-bit view specifically.

### `Delete` fails silently, and that was a real bug

If the app is running at uninstall time, `Delete` on its exe fails, sets the
error flag, and **execution simply continues** — so the uninstaller would go on
to remove the registry key and shortcuts and report success, leaving the binary
in Program Files with the Apps & Features entry that would let the user retry
already gone. No route back through the UI.

Now: retry loop with a "close the app" prompt when interactive,
`Delete /REBOOTOK` as the fallback so nothing is orphaned permanently.

`verify-installer.ps1` found this because it uninstalls immediately after
killing the app — ruder than a human tester would ever be.

### The artefact path is not what the plan said

Plan Task 30 specifies `..\build\Release\HandsFree.exe`. All three parts of
that are wrong. The real path is:

```
build\HandsFree_artefacts\Release\AZ Soundtech Hands-free.exe
```

`juce_add_gui_app` emits into `<target>_artefacts\Release\`; the file is named
after `PRODUCT_NAME`, not after the CMake target name; and that name contains
spaces, so every `File` / `CreateShortcut` / `Delete` line touching it must be
quoted.

## Not done, on purpose

- **Task 31, code signing.** Blocked on a certificate nobody has bought
  (EV, ~$300–500/yr) and on `signtool` not being installed. The exact commands
  are recorded as a commented block at the foot of `handsfree.nsi`. Do **not**
  substitute a self-signed certificate: it produces the identical SmartScreen
  warning Task 31 exists to remove, so it would look finished while achieving
  nothing.
- **ASIO.** The installer does not bundle, check for, or mention an ASIO
  driver. Redistributing Steinberg's SDK carries licence conditions and
  `external/asiosdk/` is gitignored for that reason.
- **No CMake packaging target.** Adding one would make CMake the single source
  of the build directory too, but `CMakeLists.txt` is the one file every
  parallel lane touches, so this lane stayed out of it. Worth adding once the
  lanes have landed.

## Known limitation

The uninstaller runs elevated, so `$APPDATA` resolves to the profile of
whoever answered the UAC prompt. When an administrator uninstalls on behalf of
a *different* user, that user's presets and licence are left in place rather
than removed. Leaving data behind is the safe direction to be wrong in.
