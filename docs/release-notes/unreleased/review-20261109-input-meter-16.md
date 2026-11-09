# Next alpha - setup wizard input meter covers 16 inputs

**Audio path touched:** yes, read-only. The input-meter loop in the audio
callback is bounded by `kInputMeterChannels`, raised from 8 to 16 (16 relaxed
atomic stores at most per block, only while the wizard meter is on).

**Expected level change:** none. The meter only reads input samples; no output
sample is written or changed (test `TheMeterNeverChangesAnOutputSample`).
