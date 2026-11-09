#!/usr/bin/env python3
"""Deterministic test fixtures for the candidate classifier (lane C1a).

Writes tests/fixtures/classifier-fixture-v1.json (weights, feature_set 1) and
tests/fixtures/classifier-parity-v1.json (raw inputs + expected scores from
reference_forward.py). The weights are random but SEEDED -- they are a test
fixture, not a trained model, and make no claim about real feedback.

Every number is rounded to float32 first, so C++ (float) and Python (double)
start from the identical value and any difference is arithmetic, not parsing.

Usage: make_fixture.py [--out-dir tests/fixtures]
"""
import argparse
import json
import math
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reference_forward as ref  # noqa: E402

SEED = 20261109


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def make_weights(rng):
    def layer(outs, ins):
        sigma = 1.2 / math.sqrt(ins)
        return {"weights": [[f32(rng.gauss(0.0, sigma)) for _ in range(ins)] for _ in range(outs)],
                "bias": [f32(rng.gauss(0.0, 0.1)) for _ in range(outs)]}
    offset = [0, 1.0, 0.5, 0.5, 0, 0.5, 0.75, 0.5, 0, -30, -30, -30, -30, -30, -30, -3]
    scale = [0.5, 1.0, 2.0, 2.0, 0.3, 2.0, 4.0, 2.0, 0.5, 0.05, 0.05, 0.05, 0.05, 0.05, 0.05, 0.5]
    return {"feature_set": ref.FEATURE_SET, "model": "fixture-v1-seeded-random",
            "input_offset": [f32(v) for v in offset], "input_scale": [f32(v) for v in scale],
            "dense1": layer(16, 16), "dense2": layer(8, 16), "dense3": layer(1, 8)}


def near_profile(rng, centre, falloff_db):
    out = []
    for k in range(-5, 6):
        out.append(centre if k == 0 else centre * 10.0 ** (-(abs(k) * falloff_db + rng.uniform(0, 3)) / 20.0))
    return [f32(v) for v in out]


def case(hz, peak, p, r, rr, m, pen, other, lr, near):
    return {"hz": f32(hz), "peakiness": f32(peak), "pNorm": f32(p), "rNorm": f32(r), "riseRatio": f32(rr),
            "mNorm": f32(m), "penalty": f32(pen), "hasOtherLane": other, "lrRatio": f32(lr),
            "nearMags": [f32(v) for v in near]}


def make_inputs(rng):
    flat = [1e-3] * 11
    spike = [1e-5] * 5 + [1.0] + [1e-5] * 5
    cases = [
        # clamp edges: frequency band, peakiness floor/ceiling, rise ratio, lr ratio
        case(20.0, 10.0, 0.9, 0.9, 3.0, 0.8, 1.0, False, 0.0, spike),
        case(19.0, 10.0, 0.9, 0.9, 3.0, 0.8, 1.0, False, 0.0, spike),
        case(20000.0, 10.0, 0.9, 0.9, 3.0, 0.8, 1.0, False, 0.0, spike),
        case(31000.0, 10.0, 0.9, 0.9, 3.0, 0.8, 1.0, False, 0.0, spike),
        case(1000.0, 0.5, 0.5, 0.5, 1.0, 0.5, 0.5, True, 1.0, spike),
        case(1000.0, 1.0, 0.5, 0.5, 1.0, 0.5, 0.5, True, 1.0, spike),
        case(1000.0, 1.0e4, 0.5, 0.5, 1.0, 0.5, 0.5, True, 1.0, spike),
        case(1000.0, 1.0e7, 0.5, 0.5, 1.0, 0.5, 0.5, True, 1.0, spike),
        case(440.0, 12.0, 0.7, 1.0, 1.0e-3, 0.4, 1.0, True, 1.0e-6, spike),
        case(440.0, 12.0, 0.7, 1.0, 16.0, 0.4, 1.0, True, 1.0e-3, spike),
        case(440.0, 12.0, 0.7, 1.0, 256.0, 0.4, 1.0, True, 1.0e3, spike),
        case(440.0, 12.0, 0.7, 1.0, 1.0e6, 0.4, 1.0, True, 1.0e6, spike),
        case(440.0, 12.0, 1.5, 1.5, 4.0, 1.5, 1.0, False, 7.0, spike),     # 0..1 fields above 1, lr ignored (mono)
        case(440.0, 12.0, -0.2, -0.2, 4.0, -0.2, 0.0, False, 7.0, spike),  # 0..1 fields below 0
        # neighbours louder than the centre, silence, flat spectrum, denormals
        case(2500.0, 8.0, 0.6, 0.6, 2.0, 0.6, 1.0, True, 2.0, [1.0] * 5 + [0.01] + [1.0] * 5),
        case(2500.0, 8.0, 0.6, 0.6, 2.0, 0.6, 1.0, False, 0.0, [0.0] * 11),
        case(2500.0, 8.0, 0.6, 0.6, 2.0, 0.6, 1.0, True, 1.0, flat),
        case(2500.0, 8.0, 0.6, 0.6, 2.0, 0.6, 1.0, False, 0.0, [1e-40] * 11),
        case(2500.0, 8.0, 0.6, 0.6, 2.0, 0.6, 1.0, True, 1.0, [1e-30] * 5 + [1e-20] + [1e-30] * 5),
        case(2500.0, 8.0, 0.6, 0.6, 2.0, 0.6, 1.0, False, 0.0, [1e3] * 5 + [1e7] + [1e3] * 5),
    ]
    while len(cases) < 64:
        centre = 10.0 ** rng.uniform(-5.0, 0.5)
        other = rng.random() < 0.5
        cases.append(case(
            hz=2.0 ** rng.uniform(math.log2(40.0), math.log2(16000.0)),
            peak=10.0 ** rng.uniform(0.0, 2.5), p=rng.random(), r=rng.random() * 1.2,
            rr=2.0 ** rng.uniform(-2.0, 6.0), m=rng.random(), pen=rng.choice([0.5, 1.0]),
            other=other, lr=10.0 ** rng.uniform(-2.0, 2.0) if other else 0.0,
            near=near_profile(rng, centre, rng.uniform(2.0, 14.0))))
    return cases


def main(argv):
    default_out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "fixtures")
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default=default_out)
    args = ap.parse_args(argv)
    rng = random.Random(SEED)
    weights = make_weights(rng)
    cases = []
    for inp in make_inputs(rng):
        cases.append({"input": inp, "expected": ref.score(weights, inp)})
    parity = {"feature_set": ref.FEATURE_SET, "tolerance": 1.0e-5, "cases": cases}
    os.makedirs(args.out_dir, exist_ok=True)
    for name, doc in (("classifier-fixture-v1.json", weights), ("classifier-parity-v1.json", parity)):
        with open(os.path.join(args.out_dir, name), "w", encoding="utf-8", newline="\n") as fh:
            json.dump(doc, fh, indent=1)
            fh.write("\n")
    print("wrote 417 weights params, %d parity cases" % len(cases))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
