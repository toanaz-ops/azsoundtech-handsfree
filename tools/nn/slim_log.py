#!/usr/bin/env python3
"""Shrink a session log so it can be committed (used for the synthetic logs only).

Standard library only. A real notch_set carries three 1025-value spectra (~14 KB
a line). The training pipeline reads only 11 bins of `ctx.now` around the peak,
so for the committed synthetic logs this keeps a window of +-WINDOW bins and
drops what no reader in this repo uses for training:

  * ctx.now           -> the window only; ctx gets "first_bin" (index of its
                         first value in the full spectrum). ctx.bins is KEPT as
                         the full length, so dataset.py still treats off-spectrum
                         neighbours exactly as the app does.
  * ctx.ref, ctx.other_lane_now, ctx.ref_age_ms  -> removed.

Every other line is copied through untouched. dataset.py gives the same rows for
a log and for its slimmed form (tests/test_nn_pipeline.py pins that).
Real logs are never slimmed or committed.

    python tools/nn/slim_log.py IN.jsonl OUT.jsonl
"""
from __future__ import annotations

import json
import math
import sys

WINDOW = 16


def slim_event(e, window=WINDOW):
    if e.get("ev") != "notch_set" or not isinstance(e.get("ctx"), dict):
        return e
    ctx = e["ctx"]
    now, bin_hz, bins = ctx.get("now"), ctx.get("bin_hz"), ctx.get("bins")
    hz = e.get("hz")
    if not (isinstance(now, list) and isinstance(bin_hz, (int, float)) and bin_hz > 0
            and isinstance(bins, int) and isinstance(hz, (int, float)) and math.isfinite(hz)
            and len(now) == bins and "first_bin" not in ctx):
        return e                                   # not a full-spectrum line: leave it alone
    b = int(math.floor(hz / bin_hz + 0.5))
    lo, hi = max(0, b - window), min(bins, b + window + 1)
    if lo >= hi:
        return e
    out = dict(e)
    out["ctx"] = {"bins": bins, "bin_hz": bin_hz, "first_bin": lo, "now": now[lo:hi]}
    return out


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    n = 0
    with open(argv[0], "r", encoding="utf-8") as src, \
            open(argv[1], "w", encoding="utf-8", newline="\n") as dst:
        for line in src:
            line = line.strip()
            if not line:
                continue
            dst.write(json.dumps(slim_event(json.loads(line)), separators=(",", ":"),
                                 allow_nan=False) + "\n")
            n += 1
    print("wrote %d lines to %s" % (n, argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
