# Runbook — parallel lanes with git worktrees

**Verified end-to-end on 2026-08-22** against `main` @ `f4bba43`: worktree
created, junction made, clean configure, full build of both targets, **70/70
tests passing inside the lane**, `git status` clean, a commit made and rolled
back, and the main repo confirmed unaffected afterwards.

The parallel execution plan §0 asked for exactly this check before any lane
depended on it (*"budget one throwaway lane"*). This is the result. The plan's
own two-line recipe is **incomplete** — it works only with the extra step in
§2 below, without which `git` in the lane dies outright.

---

## Why a junction at all

`external/JUCE` is a submodule: its `.git` is a 41-byte gitdir pointer file, and
the checkout is 99 MB. A fresh `git worktree` gets an **empty**
`external/JUCE` (verified: 0 entries), so `add_subdirectory` fails. Re-running
`git submodule update --init` per lane re-clones 99 MB plus history.

JUCE is read-only vendored code, so one checkout can be shared by junction.

---

## The procedure

### 1. One-time, in the main repo

```bash
git config extensions.worktreeConfig true
```

Without this, step 3 silently corrupts the main repo. See §2.

### 2. ⚠ The step the plan is missing

A junctioned submodule breaks git in the lane:

```
fatal: not a git repository: external/JUCE/../../.git/modules/external/JUCE
```

`external/JUCE/.git` holds a **relative** path, `gitdir: ../../.git/modules/...`.
In the main repo `../../` lands on the repo root, where `.git/modules/` exists.
In a worktree it lands on the worktree root, whose `.git` is a *file* pointing at
`.git/worktrees/<lane>` — no `modules/` there. The junction shares the working
files, but the pointer inside them is only valid from one location.

The fix is to tell git the submodule is not this lane's business:

```bash
git config --worktree submodule."external/JUCE".active false
git config --worktree submodule."external/JUCE".ignore all
```

**`--worktree` is not optional.** Plain `git config` writes to the *shared*
config. Doing that during this verification set `active=false` on the **main
repo**, and `git submodule status` there immediately started reporting
`-e18f7f5…` — submodule not initialised. It was caught and reverted, but a lane
setup that does it and does not check would quietly break the main checkout for
every other lane at once.

After the fix, verify **both** sides before trusting it:

```bash
cd <lane>          && git status --short          # must be clean, not fatal
cd <main repo>     && git submodule status        # must have a LEADING SPACE,
                                                  # not a leading dash
```

### 3. Create a lane

```bash
git config extensions.worktreeConfig true                    # once, main repo

git worktree add D:/hf-lanes/<lane> -b <branch>
rm -rf D:/hf-lanes/<lane>/external/JUCE                      # empty placeholder
```

```powershell
New-Item -ItemType Junction `
  -Path   'D:\hf-lanes\<lane>\external\JUCE' `
  -Target 'D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\external\JUCE'
```

```bash
cd D:/hf-lanes/<lane>
git config --worktree submodule."external/JUCE".active false
git config --worktree submodule."external/JUCE".ignore all
git status --short          # must be clean
```

### 4. Build the lane

```powershell
cmake -S D:\hf-lanes\<lane> -B D:\hf-lanes\bld-<lane> -G "Visual Studio 18 2026" -A x64
cmake --build D:\hf-lanes\bld-<lane> --config Release
ctest --test-dir D:\hf-lanes\bld-<lane> -C Release
```

Configure takes ~71 s (JUCE + a googletest fetch). Measured, not estimated.

**Two things about that command line:**

- **The generator is `Visual Studio 18 2026`, not 17 2022.** This machine has
  *Visual Studio Build Tools 2026* and no VS 2022 at all — `vswhere` reports one
  install, `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`.
  `CLAUDE.md` documents `-G "Visual Studio 17 2022"`, which fails on a clean
  configure here. It appears to work only because `build/` already holds a cache
  that records `Visual Studio 18 2026`, and `cmake --build` reads the cache
  rather than the flag. This is the same shape as the `build-task8-msvc/` defect:
  a surviving build directory hiding a broken clean configure.
- **Omitting `-G` does not work locally**, though it is correct in CI. The
  `cmake` on PATH is the MinGW/WinLibs build and defaults to Ninja, which then
  rejects `-A x64`. On the GitHub runner cmake defaults to the newest Visual
  Studio, which is why `.github/workflows/build.yml` omits `-G` and has been
  green four runs running. That is an assumption about the runner's cmake, and
  it is recorded here so nobody has to rediscover it if CI ever changes.

### 5. Build directories live outside the repo, on a short path

`D:/hf-lanes/bld-<lane>`. MSVC's FileTracker fails with `FTK1011` on long paths,
which has bitten two agents on this project. `.gitignore` also has `build-*/`, so
an in-repo lane build directory would be invisible to `git status` — the exact
condition that let a stale `JuceHeader.h` survive.

### 6. Tear down — ⚠ THE MOST DANGEROUS STEP

**Remove the junction BEFORE removing the worktree.** A junction is a directory
to every tool that walks a tree, so `git worktree remove --force` and `rm -rf`
both follow it and delete **the target** — the shared `external/JUCE` that the
main repo and every other lane are using.

This is not hypothetical. It happened during the very session that wrote this
runbook: teardown ran `git worktree remove --force` with the junction still in
place, `external/JUCE` was emptied, and `git submodule status` in the main repo
went to `-e18f7f5…`. Recovery is `git submodule update --init --recursive`, a
99 MB re-clone. Nothing was permanently lost, because the submodule SHA lives in
the index — but every lane was broken until it finished.

The rule in §"Rules for anyone working in a lane" says never write to
`external/JUCE`. Teardown *is* a write, and it is the one nobody thinks of.

```bash
# 1. Unlink the junction FIRST. rmdir removes the LINK; rm -rf follows it.
cmd //c rmdir "D:\hf-lanes\<lane>\external\JUCE"

# 2. PROVE the shared checkout survived before going any further.
ls "D:/DEV CAVE EP3/PROJECT005-AZ-handsfree/external/JUCE/modules" | wc -l
#    must print a non-zero count. If it prints 0, STOP and re-clone:
#    git submodule update --init --recursive external/JUCE

# 3. Only now is it safe to remove the worktree.
git worktree remove --force D:/hf-lanes/<lane>
git branch -D <branch>
rm -rf D:/hf-lanes/bld-<lane>
```

---

## Rules for anyone working in a lane

1. **Never write to `external/JUCE`.** The junction is shared: a write from one
   lane lands in the main repo's checkout and every other lane at once. That
   includes `git submodule update` — do not run it inside a lane.
2. **Never run plain `git config` for submodule keys in a lane.** Always
   `--worktree`. See §2.
3. **Verify the main repo's `git submodule status` after setting up a lane.**
   A leading dash means the shared config was written; stop and revert.
4. **One lane, one set of files.** The whole point is disjoint ownership; see the
   lane map below.

---

## Lane map — what can actually run at once

State: Tasks 1–11 done, plus this session's depth fix and test debt.
Open: 5, 12–32.

### Ready now, needing no owner decision

| Lane | Tasks | Owns | Caveat |
|---|---|---|---|
| **Licensing** | 27, 28, 29 | `src/app/LicenseManager.*` (new) | Task 28 posts to `license.azsoundtech.com`, which very likely does not exist yet. Build it against an injectable HTTP client — better design regardless, and the only way to test it. |
| **Installer** | 30 | `installer/` (new) | Path verified against a real build: `build/HandsFree_artefacts/Release/AZ Soundtech Hands-free.exe`. The name has spaces; the plan's NSIS `File` line does not quote it. |
| **Preset format** | 25 (partly), 26 | `src/app/PresetManager.*` (new), preset JSON | Serialization over a plain struct is standalone. Wiring it to the detector waits for the bridge. `depthDB` now works, so −18 dB / −10 dB are representable — they were not before this session. |

### Ready, but needs one yes

| Lane | Tasks | Owns |
|---|---|---|
| **AudioEngine interface** | the 11 methods in the Lane C audit §3 | `src/app/AudioEngine.*` |

Unblocks Tasks 16, 17, 18 and most of 23. Independent of the bridge design.

### Blocked on the bridge design being approved

Tasks 5, 12, 13, 14, 15, 19, 20, 22, 24, and the wiring half of 25.

### Not parallelisable — chains, not task lists

- **12 → 13 → 14 → 15.** 13 consumes 12's candidates, 14 consumes 13's notch
  state, 15 changes 14's cadence.
- **19 → 20 → 21.** 20 draws over 19; 21 optimises both.

### Blocked by something no amount of engineering fixes

- **Task 31** needs a purchased EV code-signing certificate (~$300–500/year).
- **Task 32** needs physical hardware — an Audient iD14, a Behringer Wing, a
  rehearsal room — and an 8-hour continuous run.

### The one shared file every lane touches

`CMakeLists.txt`, to add its new sources. One line per lane, and the conflict is
trivial to resolve — but land lanes one at a time rather than merging three at
once into the same source list.
