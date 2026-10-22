# Next alpha - notch commands survive the device opening

**Audio path touched:** yes (`AudioEngine::audioDeviceAboutToStart`).

**Expected level change:** none for a normal start. The only difference is in
this sequence: the app starts with no device open (ASIO not ready), a preset is
loaded or a notch is placed, then the device is picked. Before, those notches
showed as active in the table but were never applied; now they are applied the
moment the device opens. So a PA that used to be unprotected in that sequence
will now carry the notches the screen already showed. Listen at low volume the
first time you load a preset before selecting the device.
