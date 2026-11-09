#!/usr/bin/env python3
"""Reference implementation of the candidate classifier (lane C1, feature_set 1).

Standard library only, so it can run inside ctest. This file is the SPEC in
executable form: src/app/CandidateClassifier.cpp must produce the same score
for the same raw input (tolerance in the parity fixture). Both sides are
pinned: the C++ test compares against the expected scores stored in
tests/fixtures/classifier-parity-v1.json, and the ctest that runs this script
compares THIS implementation against the same stored scores.

Usage:
  reference_forward.py --weights W.json --parity P.json
      exit 0 when max |python - expected| <= tolerance in P.json, else 1.
"""
import argparse
import json
import math
import sys

FEATURE_SET = 1
NUM_FEATURES = 16
NEAR_BINS = 11
EPS = 1.0e-12
MIN_HZ, MAX_HZ = 20.0, 20000.0


def _clamp(x, lo, hi):
    return lo if x < lo else (hi if x > hi else x)


def _db_rel(mag, centre):
    return _clamp(20.0 * math.log10(max(mag, EPS) / max(centre, EPS)), -60.0, 0.0)


def raw_features(inp):
    """Returns the 16 unnormalised features, or None if any raw field is not finite."""
    fields = [inp["hz"], inp["peakiness"], inp["pNorm"], inp["rNorm"], inp["riseRatio"],
              inp["mNorm"], inp["penalty"], inp["lrRatio"]] + list(inp["nearMags"])
    if not all(math.isfinite(v) for v in fields):
        return None
    near = inp["nearMags"]
    assert len(near) == NEAR_BINS
    centre = max(near[5], EPS)
    has_other = bool(inp["hasOtherLane"])
    f = [0.0] * NUM_FEATURES
    f[0] = math.log2(_clamp(inp["hz"], MIN_HZ, MAX_HZ) / 1000.0)
    f[1] = _clamp(math.log10(max(inp["peakiness"], 1.0)), 0.0, 4.0)
    f[2] = _clamp(inp["pNorm"], 0.0, 1.0)
    f[3] = _clamp(inp["rNorm"], 0.0, 1.0)
    f[4] = _clamp(math.log2(max(inp["riseRatio"], EPS)), -4.0, 8.0)
    f[5] = _clamp(inp["mNorm"], 0.0, 1.0)
    f[6] = _clamp(inp["penalty"], 0.0, 1.0)
    f[7] = 1.0 if has_other else 0.0
    f[8] = _clamp(math.log10(max(inp["lrRatio"], EPS)), -3.0, 3.0) if has_other else 0.0
    f[9] = _db_rel(near[3], centre)
    f[10] = _db_rel(near[4], centre)
    f[11] = _db_rel(near[6], centre)
    f[12] = _db_rel(near[7], centre)
    f[13] = _db_rel((near[0] + near[1] + near[2]) / 3.0, centre)
    f[14] = _db_rel((near[8] + near[9] + near[10]) / 3.0, centre)
    f[15] = _clamp(math.log10(centre), -12.0, 6.0)
    return f


def _dense(x, w, b):
    return [sum(wi * xi for wi, xi in zip(row, x)) + bi for row, bi in zip(w, b)]


def score(weights, inp):
    """Returns the 0..1 score, or None for 'no opinion'."""
    f = raw_features(inp)
    if f is None:
        return None
    x = [(f[i] - weights["input_offset"][i]) * weights["input_scale"][i] for i in range(NUM_FEATURES)]
    h1 = [math.tanh(v) for v in _dense(x, weights["dense1"]["weights"], weights["dense1"]["bias"])]
    h2 = [math.tanh(v) for v in _dense(h1, weights["dense2"]["weights"], weights["dense2"]["bias"])]
    z = _dense(h2, weights["dense3"]["weights"], weights["dense3"]["bias"])[0]
    y = 1.0 / (1.0 + math.exp(-z)) if z > -700.0 else 0.0
    if not math.isfinite(y):
        return None
    return _clamp(y, 0.0, 1.0)


def load_weights(path):
    with open(path, "r", encoding="utf-8") as fh:
        w = json.load(fh)
    if w.get("feature_set") != FEATURE_SET:
        raise ValueError("unsupported feature_set %r" % (w.get("feature_set"),))
    return w


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--weights", required=True)
    ap.add_argument("--parity", required=True)
    args = ap.parse_args(argv)
    weights = load_weights(args.weights)
    with open(args.parity, "r", encoding="utf-8") as fh:
        parity = json.load(fh)
    tol = float(parity["tolerance"])
    worst = 0.0
    for i, case in enumerate(parity["cases"]):
        got = score(weights, case["input"])
        want = case["expected"]
        if got is None or want is None:
            if got is not want:
                print("case %d: no-opinion mismatch (got %r want %r)" % (i, got, want))
                return 1
            continue
        worst = max(worst, abs(got - want))
    print("cases=%d max_abs_error=%.3e tolerance=%.1e" % (len(parity["cases"]), worst, tol))
    return 0 if worst <= tol else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
