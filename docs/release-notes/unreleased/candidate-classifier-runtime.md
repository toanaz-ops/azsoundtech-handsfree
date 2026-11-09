# Next alpha - candidate classifier runtime (lane C1, step C1a)

**Audio path touched:** no. Nothing in `src/dsp/`, `AudioEngine` or `NotchController` changed,
and the app does not construct the new class.

**Expected level change:** none (0 dB), at every output.

## What the tester sees

Nothing. The session log is unchanged (no new fields), the console is unchanged, notch
decisions are unchanged.

## What changed underneath

- RTNeural (BSD-3-Clause, header-only, STL backend) is vendored as the submodule
  `external/RTNeural`, pinned to commit `95c3c0f`.
- A new class `CandidateClassifier` (a 16-16-8-1 network, 417 parameters) is compiled into the
  app but never called. Step C1b will run it in shadow mode: it will write a score to the log and
  will not be allowed to change a notch.
- Gate tests were added: zero allocations around `score()`, parity with a Python reference,
  NaN / inf / denormal / extreme inputs, corrupt weight files.

## Build note

Clone with `--recursive`, or run `git submodule update --init external/RTNeural`. A configure
without it now stops with a message naming that command.
