# Next alpha - candidate classifier training pipeline (lane C1, step C1c)

**Audio path touched:** no. This change adds offline tools, tests and documents. No file under
`src/`, `installer/` or the embedded model changed.

**Expected level change:** none (0 dB), at every output. The app still runs the untrained
`untrained-fixture-v1` model in shadow mode, exactly as in the previous build.

## What the tester sees

- Nothing. No screen change, no sound change, no new log field.
- No tester-visible change: there is nothing new to listen for or to report.

## What changed underneath (for the team)

- `tools/nn/` can now turn session logs with GOOD / FALSE verdicts into a training set, train the
  16-16-8-1 network offline, evaluate it on sessions it never saw, and run a gate that decides whether a
  model may be embedded in the app. The gate fails closed and rejects synthetic data by construction.
- A ctest refuses any model embedded in the app that is not an `untrained-*` placeholder unless a passing
  evaluation report sits next to it.
- A synthetic bootstrap set (`tools/nn/data/synthetic-bootstrap-v0/`) and a model trained on it exist only to
  prove the pipeline runs end to end. That model is not embedded and cannot pass the gate.
- Real session logs must not be committed (public repository); `tools/nn/data/real/` and `tools/nn/out/` are ignored.
