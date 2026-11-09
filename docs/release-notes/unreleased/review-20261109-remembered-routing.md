# Next alpha - remembered routing survives the card coming back

**Audio path touched:** no.

**Expected level change:** none. When the saved sound card was missing at start
and the operator later selects it again in the device panel, its saved routing
is re-applied (it used to be overwritten by the all-off routing of the fallback
card). A different card still just clears the mismatch.
