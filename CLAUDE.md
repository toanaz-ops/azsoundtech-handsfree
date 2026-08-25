# PROJECT005 — AZ Soundtech Hands-free (ASIO feedback elimination)

Global rules load automatically from
`D:\DEV CAVE EP3\shared\harness\RULES.md`. This file holds ONLY what is true
here. Tier 2.

## 🛑 This is live-sound DSP. A bad build reaches a PA system.

Feedback elimination runs on a real ASIO interface into a real room. An
unstable filter, a wrong gain, or a denormal blow-up does not print a stack
trace — it produces a loud noise through a speaker at a live event.

- Never change filter coefficients, gain staging, or buffer handling and call
  it done on a green build alone. A green build does not prove stability.
- State the expected level change before proposing a DSP change. If you cannot
  predict it, say so and let a human test at low volume first.
- Never remove a limiter, a clamp, a NaN/denormal check, or a bounds check to
  simplify code. See global rule 10.

## Build

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd build && ctest -C Release
```

The generator is **2026, not 2022** — this machine has only Visual Studio
Build Tools 2026 (`vswhere` reports one install, `...\Microsoft Visual
Studio\18\BuildTools`). This file said `Visual Studio 17 2022` until
2026-08-22; that command fails on a clean configure and only appeared to work
because `build/` already held a cache recording the real generator, which
`cmake --build` reads instead of the flag.

Do **not** drop `-G` locally: the `cmake` on PATH is the MinGW/WinLibs build
and defaults to Ninja, which rejects `-A x64`. CI *does* omit `-G`, because the
GitHub runner’s cmake defaults to the newest Visual Studio — see
`.github/workflows/build.yml`.

**How many builds does the change need?**

| Change | Build |
|---|---|
| docs, `memory/`, `*.md` | none |
| one `.cpp` body, no header touched | incremental, Release |
| any header, or `CMakeLists.txt` | full reconfigure + build |
| DSP path, ASIO callback, buffer size | full build **and** `ctest`, then a human listens |

**Use `build/`.** `build-review/`, `build-task8/`, `build-task8-msvc/` and
`build-verify/` are leftovers from earlier sessions. Do not create a new
`build-*` directory; do not commit any of them.

## Dependencies that are NOT in the repo

- `external/asiosdk/` — Steinberg ASIO SDK, downloaded manually for licence
  reasons. Must contain `common/asio.h`. **Never commit it.** If configure
  fails on a missing `asio.h`, that is the cause — say so, do not vendor it.
- Submodules: clone with `--recursive`. If a submodule path is empty, run
  `git submodule update --init --recursive` before blaming the build.

## GUI work is not reported without a picture

Any task that changes what the console LOOKS like ends with a rendered
screenshot handed to the owner -- not a description of the change, and not a
green build. Owner's standing instruction, 2026-08-25.

```bash
build/tools/Release/HandsFreeSnapshot.exe shots 1440 920
```

`HandsFreeSnapshot` renders `MainComponent` offscreen through
`juce::Component::createComponentSnapshot()`: no window, no screen capture, no
DPI scaling, identical every run. Send `shots/console-live.png` (and
`console-idle.png` when the empty state changed). Drop `--fast` when the notch
age ramp matters -- it needs ~23 s of wall time to be visible.

READ the image back before sending it. Every visual bug found in the rebuild --
a button with no text, a truncated CLEAR ALL, markers burying the trace, a
mojibake middle dot -- passed the whole test suite. A green build says nothing
about whether the thing is legible.

Full technique and its traps: `.claude/skills/juce-component-snapshot/SKILL.md`.

## Source of truth

1. `memory/MEMORY.md` — index of lessons. Search before re-deriving.
2. `docs/` — design notes.
3. `openspec/specs` — change through `opsx-*` commands, not by hand.

## Definition of done

1. Build command and `ctest` output pasted.
2. Anything audio-path related is marked "needs a human listen" until a human
   confirms it.
3. `memory/` note if the work taught something non-obvious, indexed in
   `memory/MEMORY.md`.
4. Commit with explicit paths. Merge only if the user said "merge".
