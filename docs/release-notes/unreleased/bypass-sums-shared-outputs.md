# Next alpha - BYPASS now sums lanes that share an output channel

**Audio path touched:** yes (`AudioEngine::audioDeviceIOCallbackWithContext`, Bypass branch).

**Expected level change:** none on any rig where each output channel has one
source (the default slot 01 `{1,2} -> {1,2}` and every one-to-one route): the
output is bit-identical to before. On a rig where two lanes are routed to the
SAME output channel, BYPASS used to pass only the last lane and silently drop the
others; now it passes their sum, the same as AUTO does. On such a rig the PA
level on that channel goes UP when you press BYPASS (the missing mic returns,
at its own level, plus the other). Check with a low master the first time.
