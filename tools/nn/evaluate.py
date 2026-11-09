#!/usr/bin/env python3
"""Evaluate a classifier weights file on a dataset, split BY SESSION (spec 8.3).

Standard library only. Inference goes through reference_forward.score, i.e. the
same arithmetic as the app (CandidateClassifier.cpp), not through numpy.

    python tools/nn/evaluate.py --dataset OUT/dataset.jsonl --weights W.json --out eval-report.json

Held-out sessions: dataset.split_sessions (deterministic: sha256 of the session
id). The model must not have seen them; train_mlp.py uses the same split.

Metrics on the held-out rows (higher score = more likely a real howl, label 1):
  * ROC-AUC of the model; ROC-AUC of the heuristic `score` logged by the app,
    on the same rows (the baseline the model has to beat);
  * at the threshold that keeps >= RETENTION of the label-1 rows: the fraction
    of label-0 rows (false notches) that would be rejected.
AUC with only one class present is "undefined" (None in the JSON), never a number.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import dataset as ds  # noqa: E402
import gate  # noqa: E402
import reference_forward as ref  # noqa: E402


def roc_auc(scores, labels):
    """Mann-Whitney AUC with average ranks for ties. None when a class is empty
    or a score is not finite (undefined: never a number, never an exception)."""
    pos = [s for s, y in zip(scores, labels) if y == 1]
    neg = [s for s, y in zip(scores, labels) if y == 0]
    if not pos or not neg:
        return None
    if any((s is None) or not math.isfinite(s) for s in scores):
        return None
    order = sorted(range(len(scores)), key=lambda i: scores[i])
    ranks = [0.0] * len(scores)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and scores[order[j + 1]] == scores[order[i]]:
            j += 1
        avg = (i + j) / 2.0 + 1.0           # 1-based average rank of the tie group
        for k in range(i, j + 1):
            ranks[order[k]] = avg
        i = j + 1
    rank_sum_pos = sum(r for r, y in zip(ranks, labels) if y == 1)
    n_pos, n_neg = len(pos), len(neg)
    return (rank_sum_pos - n_pos * (n_pos + 1) / 2.0) / (n_pos * n_neg)


def reject_at_retention(scores, labels, retention):
    """Threshold t = the k-th largest label-1 score, k = ceil(retention * P), so
    at least `retention` of label-1 rows have score >= t. Returns
    (t, retention_achieved, fraction of label-0 rows with score < t), or None."""
    pos = sorted((s for s, y in zip(scores, labels) if y == 1), reverse=True)
    neg = [s for s, y in zip(scores, labels) if y == 0]
    if not pos or not neg:
        return None
    k = max(1, int(math.ceil(retention * len(pos) - 1e-12)))
    t = pos[k - 1]
    kept = sum(1 for s in pos if s >= t)
    rejected = sum(1 for s in neg if s < t)
    return t, kept / len(pos), rejected / len(neg)


def _side(rows, scores_model, scores_base, retention):
    labels = [r["label"] for r in rows]
    out = {"rows": len(rows),
           "label1": sum(1 for y in labels if y == 1),
           "label0": sum(1 for y in labels if y == 0),
           "retention_target": retention}
    out["auc_model"] = roc_auc(scores_model, labels)
    out["auc_baseline"] = roc_auc(scores_base, labels)
    for tag, sc in (("", scores_model), ("baseline_", scores_base)):
        res = reject_at_retention(sc, labels, retention) if all(
            s is not None and math.isfinite(s) for s in sc) else None
        out[tag + "threshold"] = res[0] if res else None
        out[tag + "retention_achieved"] = res[1] if res else None
        out[tag + "reject_label0_at_retention"] = res[2] if res else None
    return out


def evaluate(rows, weights, weights_path, dataset_sha, retention=gate.RETENTION):
    if not rows:
        raise ValueError("empty dataset")
    sessions = sorted({r["session_id"] for r in rows})
    train_ids, held_ids = ds.split_sessions(sessions)
    held_set = set(held_ids)

    model = [ref.score(weights, r["raw"]) for r in rows]
    base = [r.get("score") for r in rows]
    # a missing / non-finite baseline or model score makes that metric undefined
    model = [m if (m is not None and math.isfinite(m)) else None for m in model]
    base = [b if (b is not None and math.isfinite(b)) else None for b in base]

    def pick(want_held):
        idx = [i for i, r in enumerate(rows) if (r["session_id"] in held_set) == want_held]
        return ([rows[i] for i in idx], [model[i] for i in idx], [base[i] for i in idx])

    held = _side(*pick(True), retention)
    train = _side(*pick(False), retention)

    comp = {}
    for r in rows:
        comp[r["source"]] = comp.get(r["source"], 0) + 1
    with open(weights_path, "r", encoding="utf-8") as fh:
        model_name = json.load(fh).get("model")
    return {
        "report_version": 1,
        "dataset_sha256": dataset_sha,
        "weights_sha256": gate.sha256_file(weights_path),
        "weights_model": model_name,
        "rows_total": len(rows),
        "rows_by_label": {"1": sum(1 for r in rows if r["label"] == 1),
                          "0": sum(1 for r in rows if r["label"] == 0)},
        "source_composition": dict(sorted(comp.items())),
        "sessions_total": len(sessions),
        "split": {"rule": "sha256(%s:session_id) order, first %.0f%% held out" % (
                      ds.SPLIT_SALT, ds.SPLIT_HELDOUT_FRACTION * 100),
                  "train_sessions": train_ids, "heldout_sessions": held_ids},
        "heldout": held,
        "train_side": train,           # for spotting overfitting; the gate reads only `heldout`
        "thresholds_used": dict(gate.THRESHOLDS),
        "inference": "tools/nn/reference_forward.py (same arithmetic as CandidateClassifier.cpp)",
    }


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dataset", required=True)
    ap.add_argument("--weights", required=True)
    ap.add_argument("--out", required=True, help="eval-report.json to write")
    args = ap.parse_args(argv)
    rows = ds.read_rows(args.dataset)
    weights = ref.load_weights(args.weights)
    report = evaluate(rows, weights, args.weights, ds.sha256_file(args.dataset))
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(report, fh, indent=1, sort_keys=True, allow_nan=False)
        fh.write("\n")
    h = report["heldout"]
    fmt = lambda v: "undefined" if v is None else "%.4f" % v
    print("held-out: %d rows (label1 %d, label0 %d) from sessions %s" % (
        h["rows"], h["label1"], h["label0"], report["split"]["heldout_sessions"]))
    print("AUC model %s  baseline score %s" % (fmt(h["auc_model"]), fmt(h["auc_baseline"])))
    print("at >= %.0f%% label-1 retention: label-0 rejected  model %s  baseline %s" % (
        h["retention_target"] * 100, fmt(h["reject_label0_at_retention"]),
        fmt(h["baseline_reject_label0_at_retention"])))
    print("wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
