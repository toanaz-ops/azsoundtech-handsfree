#!/usr/bin/env python3
"""The gate a classifier model must pass before it may be embedded in the app
with a meaningful score (lane C1c, spec section 8.3, decision Q24).

Standard library only. FAILS CLOSED: a missing field, a non-number, a NaN, an
out-of-range rate, a report that cannot be read, a hash that does not match --
each is a failure, never a pass. Exit 0 only when every condition holds; every
failed condition is printed.

    python tools/nn/gate.py --report R.json --weights W.json --manifest M.json --meta W.meta.json

The manifest (dataset.py) and the training metadata (train_mlp.py, `<weights>.meta.json`)
are MANDATORY. The gate ties the artefacts together: the dataset sha256 in the
report, in the manifest and in the training metadata must be the same ("trained on
A, evaluated on B" fails), and the weights sha256 in the report and in the training
metadata must be the sha256 of the weights file.

What the gate does NOT protect against: `source` is a marker the log declares about
itself (session_start.source, absent = real). The gate catches accidents (a synthetic
set, an unlabelled log, mixed files); it does not stop someone who forges a log or a
report. A human must read the manifest before a model is embedded.

Passing is NOT permission to let the network decide anything: it only means the
model may be embedded to run in shadow with a score that carries information.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys

# ---------------------------------------------------------------------------
# THE thresholds. One place. Decision Q24 of
# docs/superpowers/decisions/2026-11-09-ai-integration.md fixed them
# provisionally (09/11/2026, amended the same day); changing one is an owner
# decision, not an edit made to get a model through. evaluate.py imports
# RETENTION from here.
THRESHOLDS = {
    "min_rows": 300,                   # labelled detector notches, all sessions
    "min_sessions": 3,
    "min_heldout_rows": 60,
    "min_heldout_per_label": 10,       # stricter than Q24's "both labels": 59 vs 1 says nothing
    "min_auc": 0.80,
    "auc_margin_over_baseline": 0.05,  # model AUC >= heuristic score AUC + this (fails closed at the float edge)
    "retention": 0.95,                 # the operating point: keep >= this fraction of real howls ...
    "min_reject_label0": 0.30,         # ... and still reject >= this fraction of false notches (optimistic, in-sample)
    # Honest pair (amendment of Q24): the 95 %-retention threshold is picked on the
    # TRAIN-side rows and applied unchanged to the held-out rows; there:
    "min_heldout_retention_at_train_threshold": 0.90,
    "min_heldout_reject_at_train_threshold": 0.30,
}
RETENTION = THRESHOLDS["retention"]


def sha256_file(path):
    """sha256 of a TEXT file with CRLF read as LF: a Windows checkout with
    autocrlf rewrites line endings of a committed weights file, and that must
    not make a valid report look like it belongs to different weights."""
    with open(path, "rb") as fh:
        data = fh.read()
    return hashlib.sha256(data.replace(bytes([13, 10]), bytes([10]))).hexdigest()


def _get(d, *path):
    """Nested lookup; returns (found, value)."""
    cur = d
    for k in path:
        if not isinstance(cur, dict) or k not in cur:
            return False, None
        cur = cur[k]
    return True, cur


def _number(d, failures, *path, unit=False, count=False):
    """A finite real number at `path`, else records a failure and returns None.
    unit=True: must lie in [0, 1] (AUCs, rates). count=True: a non-negative integer."""
    name = ".".join(path)
    ok, v = _get(d, *path)
    if not ok:
        failures.append("missing field %s" % name)
        return None
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
        failures.append("field %s is not a finite number (%r)" % (name, v))
        return None
    if count and (isinstance(v, float) or v < 0):
        failures.append("field %s is not a non-negative integer (%r)" % (name, v))
        return None
    if unit and not (0.0 <= v <= 1.0):
        failures.append("field %s is outside [0, 1] (%r)" % (name, v))
        return None
    return v if count else float(v)


def _sha(d, key, failures, what):
    v = d.get(key) if isinstance(d, dict) else None
    if not isinstance(v, str) or len(v) != 64:
        failures.append("%s: missing or malformed %s" % (what, key))
        return None
    return v


def check(report, weights_path, manifest=None, meta=None, thresholds=THRESHOLDS):
    """Returns the list of failed conditions (empty list = pass)."""
    t = thresholds
    fails = []
    if not isinstance(report, dict):
        return ["report is not a JSON object"]

    # 1. every row real
    ok, comp = _get(report, "source_composition")
    rows_total = _number(report, fails, "rows_total", count=True)
    if not ok or not isinstance(comp, dict) or not comp:
        fails.append("missing field source_composition")
    else:
        others = {k: v for k, v in comp.items() if k != "real"}
        if others:
            fails.append("data is not all real: source_composition has %s" % others)
        real = comp.get("real")
        if isinstance(real, bool) or not isinstance(real, int) or real <= 0:
            fails.append("no real rows in source_composition")
        elif rows_total is not None and real != rows_total:
            fails.append("source_composition real=%s does not equal rows_total=%s" % (real, rows_total))

    # 2. size
    if rows_total is not None and rows_total < t["min_rows"]:
        fails.append("only %d labelled rows (need >= %d)" % (rows_total, t["min_rows"]))
    sessions = _number(report, fails, "sessions_total", count=True)
    if sessions is not None and sessions < t["min_sessions"]:
        fails.append("only %d sessions (need >= %d)" % (sessions, t["min_sessions"]))

    # 3. held-out set
    held = _number(report, fails, "heldout", "rows", count=True)
    h1 = _number(report, fails, "heldout", "label1", count=True)
    h0 = _number(report, fails, "heldout", "label0", count=True)
    if held is not None and held < t["min_heldout_rows"]:
        fails.append("held-out has %d rows (need >= %d)" % (held, t["min_heldout_rows"]))
    for name, n in (("label1", h1), ("label0", h0)):
        if n is not None and n < t["min_heldout_per_label"]:
            fails.append("held-out has %d rows with %s (need >= %d: both labels)"
                         % (n, name, t["min_heldout_per_label"]))
    if None not in (held, h1, h0) and h1 + h0 != held:
        fails.append("held-out label counts %d + %d do not add up to %d" % (h1, h0, held))

    # 4. discrimination
    auc = _number(report, fails, "heldout", "auc_model", unit=True)
    base = _number(report, fails, "heldout", "auc_baseline", unit=True)
    if auc is not None and auc < t["min_auc"]:
        fails.append("model AUC %.4f < %.2f" % (auc, t["min_auc"]))
    if auc is not None and base is not None and auc < base + t["auc_margin_over_baseline"]:
        fails.append("model AUC %.4f is not >= baseline score AUC %.4f + %.2f"
                     % (auc, base, t["auc_margin_over_baseline"]))

    # 5. the operating point: optimistic in-sample pair, then the honest pair
    ret = _number(report, fails, "heldout", "retention_target", unit=True)
    if ret is not None and ret < t["retention"]:
        fails.append("report measured at retention %.3f, gate needs %.2f" % (ret, t["retention"]))
    achieved = _number(report, fails, "heldout", "insample_retention", unit=True)
    if achieved is not None and achieved < t["retention"]:
        fails.append("in-sample retention %.3f < %.2f" % (achieved, t["retention"]))
    rej = _number(report, fails, "heldout", "insample_reject_label0", unit=True)
    if rej is not None and rej < t["min_reject_label0"]:
        fails.append("at >= %.0f%% in-sample retention only %.1f%% of label-0 rows rejected (need >= %.0f%%)"
                     % (t["retention"] * 100, rej * 100, t["min_reject_label0"] * 100))
    _number(report, fails, "heldout", "train_side_threshold")
    h_ret = _number(report, fails, "heldout", "retention_label1", unit=True)
    h_rej = _number(report, fails, "heldout", "reject_label0", unit=True)
    if h_ret is not None and h_ret < t["min_heldout_retention_at_train_threshold"]:
        fails.append("at the train-side threshold the held-out set keeps only %.1f%% of label-1 rows (need >= %.0f%%)"
                     % (h_ret * 100, t["min_heldout_retention_at_train_threshold"] * 100))
    if h_rej is not None and h_rej < t["min_heldout_reject_at_train_threshold"]:
        fails.append("at the train-side threshold the held-out set rejects only %.1f%% of label-0 rows (need >= %.0f%%)"
                     % (h_rej * 100, t["min_heldout_reject_at_train_threshold"] * 100))

    # 6. the artefacts belong together: weights file, report, manifest, training metadata
    rep_w = _sha(report, "weights_sha256", fails, "report")
    rep_d = _sha(report, "dataset_sha256", fails, "report")
    actual_w = None
    if rep_w is not None:
        try:
            actual_w = sha256_file(weights_path)
        except OSError as exc:
            fails.append("cannot read weights file %s (%s)" % (weights_path, exc))
        else:
            if actual_w != rep_w:
                fails.append("weights sha256 mismatch: report %s, file %s" % (rep_w[:12], actual_w[:12]))

    if not isinstance(manifest, dict):
        fails.append("manifest is mandatory (dataset manifest.json missing or unreadable)")
    else:
        man_d = _sha(manifest, "dataset_sha256", fails, "manifest")
        if rep_d is not None and man_d is not None and rep_d != man_d:
            fails.append("dataset sha256 in the report does not match the manifest")
        srcs = manifest.get("sources")
        if not isinstance(srcs, dict) or not srcs:
            fails.append("manifest: missing sources")
        elif set(srcs) - {"real"}:
            fails.append("manifest lists non-real sources %s" % sorted(set(srcs) - {"real"}))
        if rows_total is not None and manifest.get("rows") != rows_total:
            fails.append("manifest rows %r does not equal report rows_total %r" % (manifest.get("rows"), rows_total))

    if not isinstance(meta, dict):
        fails.append("training metadata is mandatory (<weights>.meta.json missing or unreadable)")
    else:
        meta_d = _sha(meta, "dataset_sha256", fails, "training metadata")
        meta_w = _sha(meta, "weights_sha256", fails, "training metadata")
        if rep_d is not None and meta_d is not None and rep_d != meta_d:
            fails.append("trained on a different dataset than evaluated: training metadata %s, report %s"
                         % (meta_d[:12], rep_d[:12]))
        if actual_w is not None and meta_w is not None and meta_w != actual_w:
            fails.append("training metadata weights sha256 does not match the weights file")
    return fails


def load_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def check_files(report_path, weights_path, manifest_path=None, meta_path=None):
    try:
        report = load_json(report_path)
    except (OSError, ValueError) as exc:
        return ["cannot read report %s (%s)" % (report_path, exc)]
    loaded = []
    for what, path in (("manifest", manifest_path), ("training metadata", meta_path)):
        if not path:
            loaded.append(None)
            continue
        try:
            loaded.append(load_json(path))
        except (OSError, ValueError) as exc:
            loaded.append(None)
            print("cannot read %s %s (%s)" % (what, path, exc), file=sys.stderr)
    return check(report, weights_path, loaded[0], loaded[1])


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--report", required=True)
    ap.add_argument("--weights", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--meta", required=True, help="the training metadata written by train_mlp.py")
    args = ap.parse_args(argv)
    fails = check_files(args.report, args.weights, args.manifest, args.meta)
    if fails:
        print("GATE FAILED (%d condition%s):" % (len(fails), "" if len(fails) == 1 else "s"))
        for f in fails:
            print("  - " + f)
        return 1
    print("GATE PASSED: model may be embedded for shadow scoring (it still may not decide anything)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
