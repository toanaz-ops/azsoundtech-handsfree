#!/usr/bin/env python3
"""Session logs (schema 2 and 3) -> classifier dataset (lane C1c, spec section 8.1).

Standard library only.

    python tools/nn/dataset.py --out OUT_DIR log1.jsonl log2.jsonl ...

Writes OUT_DIR/dataset.jsonl (one row per detector-origin notch_set that has a
verdict), OUT_DIR/candidates.jsonl (candidate_features lines: UNLABELLED, never
used for training) and OUT_DIR/manifest.json (counts, drop reasons, sha256).

How a row is rebuilt (every field of CandidateClassifier::RawInput):

  hz, peakiness, pNorm, rNorm, riseRatio, mNorm, penalty   <- notch_set fields
      hz, peakiness, p_norm, rise, rise_ratio, novelty, penalty
  hasOtherLane / lrRatio                                    <- lr_ratio present?
      (the app writes lr_ratio only when the other lane exists)
  nearMags[11]                                              <- ctx.now, see below

nearMags is NOT a field of the notch_set line. The app builds it from the
detector spectrum around the candidate bin: nearMags[k] = now[bin + k - 5], 0
outside 0..bins-1. The log carries the whole spectrum in ctx.now, and the
candidate bin is lround(hz / ctx.bin_hz) (the notch frequency is the bin plus a
sub-bin offset clamped to +-0.45 bin, so rounding recovers the bin). A line
without ctx.now, with a ctx.now of the wrong length, or with a bin outside the
spectrum is DROPPED and counted: no neighbour value is ever invented.

Two lines of train/serve skew exist and are measured, not hidden:
  * the log rounds spectra, rise_ratio and lr_ratio to 3 significant digits
    (the app uses full floats);
  * everything else (hz, peakiness, p_norm, rise, novelty, penalty) is logged
    at full precision.

`ctx` may carry an optional `first_bin` (default 0): the index in the full
spectrum of ctx.now[0]. Real logs never have it. tools/nn/slim_log.py writes it
when it shrinks a synthetic log to a window around the peak.

The verdict join is tools/logstats.py's own (logstats.summarise), imported, not
re-derived: the n-th notch_set line is the n-th entry of summarise()["notches"].
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))

import logstats  # noqa: E402  (tools/logstats.py)
import reference_forward as ref  # noqa: E402

LABEL_OF_VERDICT = {"good": 1, "false": 0}   # logstats' verdict vocabulary
SOURCES = ("real", "synthetic")
SPLIT_HELDOUT_FRACTION = 0.25
SPLIT_SALT = "hf-c1c-split-v1"


# ---------------------------------------------------------------------------
# reading

def _reject_constant(_name):
    raise ValueError("bare non-finite constant")


def load_events(path):
    """Returns (events, bad_line_count). Unlike logstats.load this counts the
    lines it cannot parse. NaN / Infinity tokens that Python's json accepts are
    kept (and caught per row as non-finite); the bare `nan` / `inf` that
    juce::JSON may print are not JSON at all and count as a bad line."""
    events, bad = [], 0
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except ValueError:
                bad += 1
    return events, bad


def session_id_of(path):
    stem = os.path.splitext(os.path.basename(path))[0]
    return stem[:-6] if stem.endswith(".jsonl") else stem


def _fin(v):
    """A finite float or None (for fields that are kept only for information)."""
    v = _num(v)
    return v if v is not None and math.isfinite(v) else None


def _num(v):
    """float or None. Booleans and strings are not numbers here."""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None
    return float(v)


def near_mags_from_ctx(ctx, hz):
    """Returns (nearMags, None) or (None, reason)."""
    if not isinstance(ctx, dict):
        return None, "no_ctx"
    now = ctx.get("now")
    if not isinstance(now, list) or not now:
        return None, "no_ctx_now"
    bin_hz = _num(ctx.get("bin_hz"))
    bins = ctx.get("bins")
    if bin_hz is None or not math.isfinite(bin_hz) or bin_hz <= 0.0:
        return None, "bad_ctx_bin_hz"
    if isinstance(bins, bool) or not isinstance(bins, int) or bins < ref.NEAR_BINS:
        return None, "bad_ctx_bins"
    first = ctx.get("first_bin", 0)
    if isinstance(first, bool) or not isinstance(first, int) or first < 0:
        return None, "bad_ctx_first_bin"
    if first + len(now) > bins:
        return None, "ctx_now_longer_than_bins"
    if "first_bin" not in ctx and len(now) != bins:      # a real log: the whole spectrum
        return None, "ctx_now_wrong_length"
    if not math.isfinite(hz):
        return None, "non_finite"
    b = int(math.floor(hz / bin_hz + 0.5))     # C++ std::lround for positive values
    if b < 0 or b >= bins:
        return None, "bin_outside_spectrum"
    out = []
    for k in range(ref.NEAR_BINS):
        i = b + k - ref.NEAR_BINS // 2
        if i < 0 or i >= bins:
            out.append(0.0)            # the app's own rule for off-spectrum neighbours
            continue
        j = i - first
        if j < 0 or j >= len(now):
            return None, "bin_outside_ctx_window"
        v = _num(now[j])
        if v is None:
            return None, "non_numeric:ctx.now"
        out.append(v)
    return out, None


def raw_input_from_notch_set(e):
    """Returns (RawInput dict, None) or (None, drop reason)."""
    def get(key):
        return _num(e.get(key))
    fields = {"hz": "hz", "peakiness": "peakiness", "pNorm": "p_norm", "rNorm": "rise",
              "riseRatio": "rise_ratio", "mNorm": "novelty", "penalty": "penalty"}
    inp = {}
    for name, key in fields.items():
        if key not in e:
            return None, "missing_field:" + key
        v = get(key)
        if v is None:
            return None, "non_numeric:" + key
        inp[name] = v
    if "lr_ratio" in e:
        lr = get("lr_ratio")
        if lr is None:
            return None, "non_numeric:lr_ratio"
        inp["hasOtherLane"], inp["lrRatio"] = True, lr
    else:
        inp["hasOtherLane"], inp["lrRatio"] = False, 0.0
    near, why = near_mags_from_ctx(e.get("ctx"), inp["hz"])
    if why:
        return None, why
    inp["nearMags"] = near
    return inp, None


def raw_input_from_candidate(e):
    fields = {"hz": "hz", "peakiness": "peakiness", "pNorm": "p_norm", "rNorm": "rise",
              "riseRatio": "rise_ratio", "mNorm": "novelty", "penalty": "penalty"}
    inp = {}
    for name, key in fields.items():
        if key not in e:
            return None, "missing_field:" + key
        v = _num(e.get(key))
        if v is None:
            return None, "non_numeric:" + key
        inp[name] = v
    if "lr_ratio" in e:
        lr = _num(e.get("lr_ratio"))
        if lr is None:
            return None, "non_numeric:lr_ratio"
        inp["hasOtherLane"], inp["lrRatio"] = True, lr
    else:
        inp["hasOtherLane"], inp["lrRatio"] = False, 0.0
    near = e.get("near")
    if not isinstance(near, list) or len(near) != ref.NEAR_BINS:
        return None, "bad_near"
    vals = [_num(v) for v in near]
    if any(v is None for v in vals):
        return None, "non_numeric:near"
    inp["nearMags"] = vals
    return inp, None


def _finish_row(inp):
    """Features through the shared reference code; (features, None) or (None, reason)."""
    f = ref.raw_features(inp)
    if f is None:
        return None, "non_finite"
    if not all(math.isfinite(v) for v in f):
        return None, "non_finite"
    # Stored at 10 significant digits: log2 / log10 come from the platform libm,
    # which may differ in the last bit between glibc and the Windows CRT, and the
    # dataset sha256 must not depend on that. (The app holds float32 anyway.)
    return [float("%.10g" % v) for v in f], None


# ---------------------------------------------------------------------------
# one session

def build_session(events, session_id):
    """Returns (rows, candidate_rows, stats) for one parsed log."""
    summary = logstats.summarise(events)
    header = summary["header"]
    source = header.get("source", "real")
    if source not in SOURCES:
        source = "unknown"          # never let a typo pass as "real" (the gate needs every row real)
    notch_events = [e for e in events if e.get("ev") == "notch_set"]
    notches = summary["notches"]
    if len(notch_events) != len(notches):          # logstats contract: one record per notch_set
        raise RuntimeError("logstats.summarise no longer returns one notch per notch_set")

    drops, rows = {}, []

    def drop(reason):
        drops[reason] = drops.get(reason, 0) + 1

    for e, n in zip(notch_events, notches):
        if n["origin"] != "detector":
            drop("origin_" + str(n["origin"]))
            continue
        if n["verdict"] is None:
            drop("no_verdict")
            continue
        label = LABEL_OF_VERDICT.get(n["verdict"])
        if label is None:
            drop("unknown_verdict")
            continue
        inp, why = raw_input_from_notch_set(e)
        if why:
            drop(why)
            continue
        feats, why = _finish_row(inp)
        if why:
            drop(why)
            continue
        row = {"session_id": session_id, "source": source, "label": label,
               "t": _fin(e.get("t")), "raw": inp, "features": feats,
               "score": _fin(e.get("score"))}
        nn = _num(e.get("nn_score"))
        if nn is not None and math.isfinite(nn):
            row["nn_score"] = nn
        rows.append(row)

    cand_rows, cand_drops = [], {}
    for e in events:
        if e.get("ev") != "candidate_features":
            continue
        inp, why = raw_input_from_candidate(e)
        feats, why2 = (None, None)
        if not why:
            feats, why2 = _finish_row(inp)
        if why or why2:
            r = why or why2
            cand_drops[r] = cand_drops.get(r, 0) + 1
            continue
        c = {"session_id": session_id, "source": source, "label": None,
             "t": _fin(e.get("t")), "raw": inp, "features": feats, "score": _fin(e.get("score"))}
        nn = _num(e.get("nn_score"))
        if nn is not None and math.isfinite(nn):
            c["nn_score"] = nn
        cand_rows.append(c)

    stats = {
        "session_id": session_id, "source": source,
        "log_schema": header.get("log_schema", 1),
        "nn_model": header.get("nn_model"),
        "notch_sets": len(notch_events),
        "rows": len(rows),
        "label1": sum(1 for r in rows if r["label"] == 1),
        "label0": sum(1 for r in rows if r["label"] == 0),
        "dropped": dict(sorted(drops.items())),
        "candidates": len(cand_rows),
        "candidates_dropped": dict(sorted(cand_drops.items())),
    }
    return rows, cand_rows, stats


# ---------------------------------------------------------------------------
# whole dataset

def _dump_jsonl(path, rows):
    h = hashlib.sha256()
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for r in rows:
            line = json.dumps(r, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n"
            fh.write(line)
            h.update(line.encode("utf-8"))
    return h.hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_dataset(paths, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    all_rows, all_cands, sessions = [], [], []
    seen = set()
    bad_total = 0
    for p in sorted(paths):
        sid = session_id_of(p)
        if sid in seen:
            raise ValueError("two logs share the session id %r; rename one" % sid)
        seen.add(sid)
        events, bad = load_events(p)
        rows, cands, stats = build_session(events, sid)
        stats["unparsable_lines"] = bad
        bad_total += bad
        all_rows += rows
        all_cands += cands
        sessions.append(stats)

    ds_path = os.path.join(out_dir, "dataset.jsonl")
    cand_path = os.path.join(out_dir, "candidates.jsonl")
    ds_sha = _dump_jsonl(ds_path, all_rows)
    cand_sha = _dump_jsonl(cand_path, all_cands)

    drops = {}
    for s in sessions:
        for k, v in s["dropped"].items():
            drops[k] = drops.get(k, 0) + v
    manifest = {
        "manifest_version": 1,
        "dataset_file": "dataset.jsonl",
        "dataset_sha256": ds_sha,
        "candidates_file": "candidates.jsonl",
        "candidates_sha256": cand_sha,
        "rows": len(all_rows),
        "label1": sum(s["label1"] for s in sessions),
        "label0": sum(s["label0"] for s in sessions),
        "sources": _count_by(all_rows, "source"),
        "candidates": len(all_cands),
        "unparsable_lines": bad_total,
        "dropped": dict(sorted(drops.items())),
        "sessions": sessions,
    }
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True, allow_nan=False)
        fh.write("\n")
    return manifest


def _count_by(rows, key):
    out = {}
    for r in rows:
        out[r[key]] = out.get(r[key], 0) + 1
    return dict(sorted(out.items()))


def read_rows(path):
    rows = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


# ---------------------------------------------------------------------------
# the split: shared by train_mlp.py and evaluate.py so both see the same sets

def split_sessions(session_ids, heldout_fraction=SPLIT_HELDOUT_FRACTION, salt=SPLIT_SALT):
    """Deterministic split BY SESSION. Returns (train_ids, heldout_ids), each
    sorted. Sessions are ordered by sha256(salt:id); the first
    max(1, round(fraction * n)) go to the held-out side. Needs >= 2 sessions."""
    ids = sorted(set(session_ids))
    if len(ids) < 2:
        raise ValueError("need at least 2 sessions to split by session, got %d" % len(ids))
    ranked = sorted(ids, key=lambda s: hashlib.sha256((salt + ":" + s).encode("utf-8")).hexdigest())
    n_held = max(1, int(round(heldout_fraction * len(ids))))
    n_held = min(n_held, len(ids) - 1)
    return sorted(ranked[n_held:]), sorted(ranked[:n_held])


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="session-*.jsonl files")
    ap.add_argument("--out", required=True, help="output directory (created)")
    args = ap.parse_args(argv)
    m = build_dataset(args.logs, args.out)
    print("rows %d (label1 %d, label0 %d)  candidates %d  sessions %d" % (
        m["rows"], m["label1"], m["label0"], m["candidates"], len(m["sessions"])))
    print("sources %s" % m["sources"])
    if m["dropped"]:
        print("dropped %s" % m["dropped"])
    print("dataset_sha256 %s" % m["dataset_sha256"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
