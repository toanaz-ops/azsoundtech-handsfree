# Next alpha - small engine / detector fixes (audit lows)

**Audio path touched:** yes, one branch in `AudioEngine::audioDeviceIOCallbackWithContext`.

**Expected level change:** none. The only callback change releases a pending
soundcheck ramp-out when the armed output channel does not exist in the current
callback (nothing is audible on such a channel). Everything else is off the audio
path: detector-thread allocations removed, thread-stop timeouts raised to 5 s,
`AudioEngine::stop()` now tears down after a device error, and the soundcheck
reference sweep is analysed once per run instead of once per target.
