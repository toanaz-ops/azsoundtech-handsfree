# Next alpha - COPILOT panel in Geek mode (lane L2a)

**Audio path touched:** no. Nothing in `src/dsp/`, `AudioEngine` or `NotchController`
changed.

**Expected level change:** none (0 dB). The panel only reads the session log.

## What the tester sees

- Geek mode, lower left, under ACTIVE NOTCHES: a new `COPILOT` panel with a
  `PHÂN TÍCH` button.
- Pressing it reads the current session log and lists plain-language remarks about
  the show so far: a frequency that keeps coming back, a notch at its depth ceiling,
  notches stacked close together, a burst of notches, a high share of FALSE verdicts,
  notches nobody judged yet, a log that lost events.
- Remarks never tell you to raise gain, raise the depth ceiling, or switch feedback
  protection off.
- It runs only when the button is pressed. Nothing is sent anywhere: no network, no
  API key, no AI model in this step.

## How it works

The installer now also ships `copilot\HandsFreeCopilot\HandsFreeCopilot.exe`
(about 19 MB). The app starts it as a separate low-priority process, waits up to
30 s, and shows what it prints. If that process is missing, slow or blocked by
antivirus, the panel shows one status line and the app carries on; audio is not
involved.

## Please check

1. Run a session until at least three notches are set, judge one FALSE, press
   `PHÂN TÍCH`. Do the remarks match what happened?
2. First press after install: how long until remarks appear? (Antivirus scanning
   can make the first run slow.)
3. Does your antivirus complain about `HandsFreeCopilot.exe`?
4. Resize the window short: the panel should disappear and ACTIVE NOTCHES should
   look exactly as before.

## Known limits

- The "ask AI" row is a separate change, see `copilot-ask-ai.md` (lane L2b). `PHÂN TÍCH` itself
  still uses no network and no key.
- The panel does not refresh by itself.
