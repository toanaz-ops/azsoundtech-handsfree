# Parallel lane briefs — 2026-08-22

Four lanes that can run **at the same time, in separate sessions**, against
`main` @ `194a090`. Each brief is self-contained: a session starting cold can
execute it without reading this project's history.

| Lane | Tasks | Brief | Blocked by |
|---|---|---|---|
| **GUI device & status** | 16, 17, 18, 23 (partly) | [lane-gui-device.md](lane-gui-device.md) | nothing — unblocked 2026-08-22 |
| **Licensing** | 27, 28, 29 | [lane-licensing.md](lane-licensing.md) | nothing |
| **Installer** | 30 | [lane-installer.md](lane-installer.md) | nothing (31 needs a bought certificate) |
| **Preset format** | 25 (format half), 26 | [lane-presets.md](lane-presets.md) | nothing for the format; wiring waits on the bridge |

**Not in any lane, and deliberately so:** Tasks 5, 12, 13, 14, 15, 19, 20, 22, 24
and the wiring half of 25 all wait on the audio↔detector bridge design
(`docs/superpowers/specs/2026-08-22-audio-detector-bridge-design.md`), which is
a DRAFT awaiting the owner. Do not start them.

`12 → 13 → 14 → 15` and `19 → 20 → 21` are **chains, not task lists** — each
consumes the previous one's output. Splitting them across sessions produces
merge conflicts, not parallelism.

---

## Before you start: set up your lane

**Do not work in the main checkout.** Two writers in one working tree overwrite
each other — `git checkout` moves the ground under whoever else is reading it.

Follow **`docs/superpowers/runbooks/parallel-lanes.md`** exactly. It is verified
end-to-end (clean configure, both targets, 75/75 tests inside a lane) and it
carries three traps that will cost you an hour each if you improvise:

1. The JUCE junction alone **kills git in the lane** until you set
   `git config --worktree submodule."external/JUCE".{active,ignore}`.
2. `git config` **without** `--worktree` writes to the shared config and breaks
   the **main** repo for every other lane.
3. **Teardown is a write.** Unlink the junction *before* removing the worktree,
   or you delete the shared JUCE checkout. This has already happened once.

Build with `-G "Visual Studio 18 2026"`. This machine has no VS 2022, whatever
older docs say.

---

## Ground rules — these bind every lane

**This is live-sound DSP. A bad build reaches a PA system.** See `CLAUDE.md`.
If you change filter coefficients, gain staging or buffer handling, state the
expected level change before you ship it, and say plainly if you cannot predict
it.

**Verify by execution, never by reading.** This project has twice shipped a
"build verified, tests green" claim that was false — once with the evidence
living in an uncommitted build directory over a HEAD that could not compile.
Run the build. Run `ctest`. Quote the output.

**Report which tests are NEW and what production change would break each one.**
Do not report the suite total as your evidence. The ledger already contains a
task recorded as "21/21 PASS" while nothing anywhere instantiated the class it
was testing.

**Test-first.** Write the failing test, watch it fail *for the right reason*,
then implement. In C++ a missing symbol is a compile error — that counts as a
red, and it is worth quoting.

**Your brief may be wrong.** Spec > plan > brief. Every brief in this project
has carried that line, and every implementer who used it overturned at least one
claim their brief made. Two of the corrections in the briefs beside this file
exist because someone checked instead of assuming. **If this brief contradicts
the code, stop and report rather than implementing what you believe is wrong.**

**Do not touch another lane's files.** Each brief names what it owns.
`CMakeLists.txt` is the one file every lane edits — one line each, to add
sources. Keep that edit to one line and land lanes one at a time.

---

## When you are done

Push your branch. Write a short report next to this file naming:

- what you built, and the commit range
- the new tests, and the production change that makes each fail
- **anything in the plan or this brief that turned out to be wrong**
- what you did not do, and why
