# Next alpha - startup and restart hygiene (audit lows)

**Audio path touched:** no (message-thread code only; the audio callback is unchanged).

**Expected level change:** none.

- Detector and lane M poll threads no longer start when no audio device opened
  (idle CPU with no device drops; see the PR for before/after numbers). They start
  from the restart hooks as soon as a device does.
- Start-up no longer calls `setWidth` / `setSampleRate` on detectors the restart
  hooks had already started.
- After a driver-initiated restart the tap rings are drained and the drop counters
  rebased before the detectors relaunch, so the one-turn gap cannot produce false
  splice-drop counts.
- Leaving the wizard for Geek mode uses a public `SetupWizard::cancel()` instead of
  a test accessor.
