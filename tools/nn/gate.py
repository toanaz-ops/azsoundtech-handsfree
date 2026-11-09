#!/usr/bin/env python3
"""The gate a classifier model must pass before it may be embedded in the app
with a meaningful score (lane C1c, spec section 8.3, decision Q24).

Standard library only. FAILS CLOSED: a missing field, a non-number, a NaN, a
report that cannot be read, a hash that does not match -- each is a failure,
never a pass. Exit 0 only when every condition holds; every failed condition is
printed.

    python tools/nn/gate.py --report eval-report.json --weights W.json [--manifest manifest.json]

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
# provisionally (09/11/2026); changing one is an owner decision, not an edit
# made to get a model through. evaluate.py imports RETENTION from here.
THRESHOLDS = {
    "min_rows": 300,                   # labelled detector notches, all sessions
    "min_sessions": 3,
    "min_heldout_rows": 60,
    "min_heldout_per_label": 10,       # stricter than Q24's "both labels": 59 vs 1 says nothing
    "min_auc": 0.80,
    "auc_margin_over_baseline": 0.05,  # model AUC >= heuristic score AUC + this
    "retention": 0.95,                 # keep >= this fraction of real howls ...
    "min_reject_label0": 0.30,         # ... and still reject >= this fraction of false notches
}
RETENTION = THRESHOLDS["retention"]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _get(d, *path):
    """Nested lookup; returns (found, value)."""
    cur = d
    for k in path:
        if not isinstance(cur, dict) or k not in cur:
            return False, None
        cur = cur[k]
    return True, cur


def _number(d, failures, *path):
    """A finite real number at `path`, else records a failure and returns None."""
    name = ".".join(path)
    ok, v = _get(d, *path)
    if not ok:
        failures.append("missing field %s" % name)
        return None
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
        failures.append("field %s is not a finite number (%r)" % (name, v))
        return None
    return float(v)


def check(report, weights_path, manifest=None, thresholds=THRESHOLDS):
    """Returns the list of failed conditions (empty list = pass)."""
    t = thresholds
    fails = []
    if not isinstance(report, dict):
        return ["report is not a JSON object"]

    # 1. every row real
    ok, comp = _get(report, "source_composition")
    rows_total = _number(report, fails, "rows_total")
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
    sessions = _number(report, fails, "sessions_total")
    if sessions is not None and sessions < t["min_sessions"]:
        fails.append("only %d sessions (need >= %d)" % (sessions, t["min_sessions"]))

    # 3. held-out set
    held = _number(report, fails, "heldout", "rows")
    h1 = _number(report, fails, "heldout", "label1")
    h0 = _number(report, fails, "heldout", "label0")
    if held is not None and held < t["min_heldout_rows"]:
        fails.append("held-out has %d rows (need >= %d)" % (held, t["min_heldout_rows"]))
    for name, n in (("label1", h1), ("label0", h0)):
        if n is not None and n < t["min_heldout_per_label"]:
            fails.append("held-out has %d rows with %s (need >= %d: both labels)"
                         % (n, name, t["min_heldout_per_label"]))
    if None not in (held, h1, h0) and h1 + h0 != held:
        fails.append("held-out label counts %d + %d do not add up to %d" % (h1, h0, held))

    # 4. discrimination
    auc = _number(report, fails, "heldout", "auc_model")
    base = _number(report, fails, "heldout", "auc_baseline")
    if auc is not None and auc < t["min_auc"]:
        fails.append("model AUC %.4f < %.2f" % (auc, t["min_auc"]))
    if auc is not None and base is not None and auc < base + t["auc_margin_over_baseline"]:
        fails.append("model AUC %.4f is not >= baseline score AUC %.4f + %.2f"
                     % (auc, base, t["auc_margin_over_baseline"]))

    # 5. the operating point
    ret = _number(report, fails, "heldout", "retention_target")
    if ret is not None and ret < t["retention"]:
        fails.append("report measured at retention %.3f, gate needs %.2f" % (ret, t["retention"]))
    achieved = _number(report, fails, "heldout", "retention_achieved")
    if achieved is not None and achieved < t["retention"]:
        fails.append("achieved retention %.3f < %.2f" % (achieved, t["retention"]))
    rej = _number(report, fails, "heldout", "reject_label0_at_retention")
    if rej is not None and rej < t["min_reject_label0"]:
        fails.append("at >= %.0f%% retention only %.1f%% of label-0 rows rejected (need >= %.0f%%)"
                     % (t["retention"] * 100, rej * 100, t["min_reject_label0"] * 100))

    # 6. the report belongs to THIS weights file (and this dataset)
    ok, rep_sha = _get(report, "weights_sha256")
    if not ok or not isinstance(rep_sha, str):
        fails.append("missing field weights_sha256")
    else:
        try:
            actual = sha256_file(weights_path)
        except OSError as exc:
            fails.append("cannot read weights file %s (%s)" % (weights_path, exc))
        else:
            if actual != rep_sha:
                fails.append("weights sha256 mismatch: report %s, file %s" % (rep_sha[:12], actual[:12]))
    if manifest is not None:
        ok, ds_sha = _get(report, "dataset_sha256")
        if not ok or not isinstance(ds_sha, str):
            fails.append("missing field dataset_sha256")
        elif not isinstance(manifest, dict) or manifest.get("dataset_sha256") != ds_sha:
            fails.append("dataset sha256 in the report does not match the manifest")
        elif isinstance(manifest.get("sources"), dict) and set(manifest["sources"]) - {"real"}:
            fails.append("manifest lists non-real sources %s" % sorted(set(manifest["sources"]) - {"real"}))
    return fails


def load_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def check_files(report_path, weights_path, manifest_path=None):
    try:
        report = load_json(report_path)
    except (OSError, ValueError) as exc:
        return ["cannot read report %s (%s)" % (report_path, exc)]
    manifest = None
    if manifest_path:
        try:
            manifest = load_json(manifest_path)
        except (OSError, ValueError) as exc:
            return ["cannot read manifest %s (%s)" % (manifest_path, exc)]
    return check(report, weights_path, manifest)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--report", required=True)
    ap.add_argument("--weights", required=True)
    ap.add_argument("--manifest")
    args = ap.parse_args(argv)
    fails = check_files(args.report, args.weights, args.manifest)
    if fails:
        print("GATE FAILED (%d condition%s):" % (len(fails), "" if len(fails) == 1 else "s"))
        for f in fails:
            print("  - " + f)
        return 1
    print("GATE PASSED: model may be embedded for shadow scoring (it still may not decide anything)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
