# Next alpha - sample-rate change hygiene in the detector

**Audio path touched:** no (detector thread and model only; the audio callback is unchanged).

**Expected level change:** 0 dB in steady state. Only the first ~32 ms after a
sample-rate switch changes: the detector analysis window is now flushed instead
of mixing old-rate samples into the first spectra, so a notch can no longer be
placed from a frequency that never rang. Notches at or above the new Nyquist
(for example a 23 kHz notch after 96 -> 44.1 kHz) are now removed from the model
with reason `rate_change` instead of showing Active while the chain is Idle.
