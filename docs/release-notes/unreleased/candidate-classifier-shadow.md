# Next alpha - candidate classifier in shadow mode (lane C1, step C1b)

**Audio path touched:** no. The classifier is only ever asked for a number; nothing reads that
number for a decision, a threshold, a gain or a filter.

**Expected level change:** none (0 dB), at every output. The test
`ShadowClassifier.DecisionsAreIdenticalWithAndWithoutClassifier` replays one fixed signal with no
classifier, with the embedded model and with weights forced to a score of about 0 and about 1,
and requires the identical sequence of notch commands in all four.

## What the tester sees

- Nothing on screen. Nothing audible. Notch placement, depth, release and the verdict buttons
  behave as before.
- The session log (`%APPDATA%\AZSoundtech\HandsFree\logs\`) now has `log_schema: 3`:
  - `session_start` carries `nn_model` (`untrained-fixture-v1`) and `nn_feature_set`.
  - `candidate_features` and detector-placed `notch_set` lines carry `nn_score` (0..1). The field
    is absent, never 0, when the classifier had no opinion.
  - `session_end` carries `nn_calls`, `nn_us_p99` and `nn_us_max` (microseconds per score call).
- Nothing leaves the machine.

## The score means nothing yet

The embedded model is the **untrained** C1a fixture (random weights, id `untrained-fixture-v1`).
Its `nn_score` says nothing about feedback versus music. It is in this build only to prove the
inference path runs on a tester's machine and costs almost nothing. A trained model needs the
GOOD / FALSE verdicts from real sessions (lane C, 300 verdicts from 3 sessions).

## What changed underneath

- The model is embedded in the binary (`models/candidate-classifier-untrained-v1.json`); the
  installer is unchanged.
- One `CandidateClassifier` per detector controller, loaded in the `MainComponent` constructor
  while every detector thread is stopped. If the model fails to load, the app runs with no
  classifier and writes no `nn_*` fields.
- `SessionLogger` gained a generic hook for extra `session_end` fields.
- `tools/logstats.py` counts `nn_score` lines and prints the model id; schema 2 logs read the same.
