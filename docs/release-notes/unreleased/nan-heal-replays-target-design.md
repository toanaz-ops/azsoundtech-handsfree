# Next alpha - NaN self-heal lands depth ramps on their target

**Audio path touched:** yes (`AudioEngine::audioDeviceIOCallbackWithContext`, the
non-finite-output branch, and `NotchChain`).

**Expected level change:** none in normal operation - the branch only runs when a
notch chain produces a non-finite sample, which should not happen. When it does,
the chain now recovers with every Active notch at its stored (target) depth,
where before a notch whose depth ramp was in flight stayed frozen at an
intermediate, shallower-than-reported depth until the next retune or device
restart. So after a heal the PA can see up to the full target attenuation
instead of a partial one; it cannot see a boost (the ramp argument in Biquad.h
still holds, and the replayed design is the same one the model reports).
