# Next alpha - session log records near-miss candidates (lane C0)

**Audio path touched:** no (detector thread only).

**Expected level change:** none (0 dB). No threshold, constant or ordering in the
detector changed; the placement decision (`score > kConfirmScore`, the persistence
streak, `placeConfirmed`) is untouched. This lane only writes more to the session log.

What testers will notice: session logs (`session-*.jsonl`) carry a new
`candidate_features` line for candidates that nearly became a notch but did not
(score at least 0.35 on the 0..1 scale, at most 1 line per second per slot and
1 per 2 s per frequency), `notch_set` lines gain `rise_ratio` and (stereo)
`lr_ratio`, `session_start` gains `log_schema: 2`, and `session_end` gains
`dropped_candidates`. About 400 bytes per candidate line, roughly 1.4 MB/hour
per slot at the worst case of one line per second. `tools/logstats.py` counts
the new lines; older logs still read exactly as before.
