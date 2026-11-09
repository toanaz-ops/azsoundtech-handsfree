#!/usr/bin/env python3
"""Train the candidate classifier MLP offline (lane C1c, spec section 8.2).

Needs numpy; NEVER run by ctest. Architecture is the app's: 16 -> 16 tanh ->
8 tanh -> 1 sigmoid (417 parameters), input standardisation exported as
input_offset / input_scale in the convention CandidateClassifier.cpp uses:

    x_norm = (feature - input_offset) * input_scale        (a MULTIPLY: scale = 1/std)

    python tools/nn/train_mlp.py --dataset OUT/dataset.jsonl --name my-model-v1 \
        --out tools/nn/out/my-model-v1.json

The held-out sessions of dataset.split_sessions are never touched (they belong
to evaluate.py). Inside the rest, a validation split BY SESSION drives early
stopping. Fixed seed, Adam, class-weighted binary cross-entropy, full batch.

The weights file is exactly what CandidateClassifier::parseWeights reads, and is
validated here against that parser's rules (finite, |w| <= 1000, 1e-6 <= |scale|
<= 1e6, |offset| <= 1e6) BEFORE it is written. Also written next to it:
  <out>.meta.json    dataset sha256, seed, split, epochs, numpy version, ...
  <out minus .json>.parity.json   a few inputs with the reference score, in the
                                  format of tests/fixtures/classifier-parity-v1.json
"""
from __future__ import annotations

import os

# Reproducibility: single-threaded BLAS, set BEFORE numpy is imported.
for _v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")

import argparse  # noqa: E402
import json  # noqa: E402
import math  # noqa: E402
import sys  # noqa: E402

import numpy as np  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import dataset as ds  # noqa: E402
import reference_forward as ref  # noqa: E402

N_IN, N_H1, N_H2 = ref.NUM_FEATURES, 16, 8
# Limits of CandidateClassifier::parseWeights / validate (CandidateClassifier.cpp).
MAX_ABS_WEIGHT = 1000.0
MAX_ABS_XFORM = 1.0e6
MIN_ABS_SCALE = 1.0e-6
# We stay well inside the parser's weight bound; hitting it means divergence.
WEIGHT_CLIP = 100.0
MIN_STD = 1.0e-6          # below this a feature is treated as constant (scale 1)

DEFAULTS = dict(seed=20261109, max_epochs=600, patience=40, lr=0.01, l2=1.0e-4,
                val_fraction=0.25)


def standardise(x):
    mean = x.mean(axis=0)
    std = x.std(axis=0)
    scale = np.where(std < MIN_STD, 1.0, 1.0 / np.maximum(std, MIN_STD))
    return mean, scale


def init_params(rng):
    def glorot(n_out, n_in):
        lim = math.sqrt(6.0 / (n_in + n_out))
        return rng.uniform(-lim, lim, size=(n_out, n_in))
    return {"w1": glorot(N_H1, N_IN), "b1": np.zeros(N_H1),
            "w2": glorot(N_H2, N_H1), "b2": np.zeros(N_H2),
            "w3": glorot(1, N_H2), "b3": np.zeros(1)}


def forward(p, x):
    h1 = np.tanh(x @ p["w1"].T + p["b1"])
    h2 = np.tanh(h1 @ p["w2"].T + p["b2"])
    z = (h2 @ p["w3"].T + p["b3"])[:, 0]
    return h1, h2, z


def _sigmoid(z):
    return 1.0 / (1.0 + np.exp(-np.clip(z, -60.0, 60.0)))


def loss_and_grads(p, x, y, sw, l2):
    n = x.shape[0]
    h1, h2, z = forward(p, x)
    q = _sigmoid(z)
    eps = 1.0e-12
    ll = -(y * np.log(q + eps) + (1.0 - y) * np.log(1.0 - q + eps))
    loss = float((sw * ll).sum() / sw.sum())
    reg = 0.5 * l2 * sum(float((p[k] ** 2).sum()) for k in ("w1", "w2", "w3"))
    dz = sw * (q - y) / sw.sum()                       # d loss / d z
    g = {}
    g["w3"] = (dz[:, None] * h2).sum(axis=0, keepdims=True) + l2 * p["w3"]
    g["b3"] = np.array([dz.sum()])
    dh2 = dz[:, None] * p["w3"][0][None, :] * (1.0 - h2 ** 2)
    g["w2"] = dh2.T @ h1 + l2 * p["w2"]
    g["b2"] = dh2.sum(axis=0)
    dh1 = (dh2 @ p["w2"]) * (1.0 - h1 ** 2)
    g["w1"] = dh1.T @ x + l2 * p["w1"]
    g["b1"] = dh1.sum(axis=0)
    return loss + reg, loss, g


def class_weights(y):
    n1 = float((y == 1).sum())
    n0 = float((y == 0).sum())
    n = n1 + n0
    w1 = n / (2.0 * n1) if n1 else 0.0
    w0 = n / (2.0 * n0) if n0 else 0.0
    return np.where(y == 1, w1, w0)


def fit(x_tr, y_tr, x_va, y_va, seed, max_epochs, patience, lr, l2):
    """Adam, full batch, early stopping on the validation loss. Returns
    (params, info). Deterministic for a given seed and data."""
    rng = np.random.default_rng(seed)
    p = init_params(rng)
    m = {k: np.zeros_like(v) for k, v in p.items()}
    v = {k: np.zeros_like(val) for k, val in p.items()}
    b1, b2, eps = 0.9, 0.999, 1.0e-8
    sw_tr, sw_va = class_weights(y_tr), class_weights(y_va)
    best = {"loss": math.inf, "epoch": 0, "p": {k: a.copy() for k, a in p.items()}}
    history = []
    stopped_early = False
    epoch = 0
    for epoch in range(1, max_epochs + 1):
        _, tr_loss, g = loss_and_grads(p, x_tr, y_tr, sw_tr, l2)
        for k in p:
            m[k] = b1 * m[k] + (1 - b1) * g[k]
            v[k] = b2 * v[k] + (1 - b2) * g[k] ** 2
            mh = m[k] / (1 - b1 ** epoch)
            vh = v[k] / (1 - b2 ** epoch)
            p[k] = p[k] - lr * mh / (np.sqrt(vh) + eps)
        _, _, zv = forward(p, x_va)
        qv = _sigmoid(zv)
        va_loss = float((sw_va * -(y_va * np.log(qv + 1e-12) + (1 - y_va) * np.log(1 - qv + 1e-12))).sum()
                        / sw_va.sum())
        history.append((tr_loss, va_loss))
        if va_loss < best["loss"] - 1.0e-6:
            best = {"loss": va_loss, "epoch": epoch, "p": {k: a.copy() for k, a in p.items()}}
        elif epoch - best["epoch"] >= patience:
            stopped_early = True
            break
    info = {"epochs_run": epoch, "best_epoch": best["epoch"], "best_val_loss": best["loss"],
            "stopped_early": stopped_early, "final_train_loss": history[-1][0]}
    return best["p"], info


def export_weights(p, offset, scale, name):
    """Builds the JSON document and enforces the C++ parser's limits. Values go
    through float32 so what is stored is exactly what the app will hold."""
    f32 = lambda a: [float(v) for v in np.asarray(a, dtype=np.float32).ravel()]
    clipped = 0
    q = {}
    for k, a in p.items():
        a = np.asarray(a, dtype=np.float64)
        if not np.all(np.isfinite(a)):
            raise ValueError("non-finite weights in %s: training diverged" % k)
        n = int((np.abs(a) > WEIGHT_CLIP).sum())
        clipped += n
        q[k] = np.clip(a, -WEIGHT_CLIP, WEIGHT_CLIP)
    off = np.asarray(offset, dtype=np.float64)
    sc = np.asarray(scale, dtype=np.float64)
    if not (np.all(np.isfinite(off)) and np.all(np.isfinite(sc))):
        raise ValueError("non-finite input_offset / input_scale")
    if (np.abs(off) > MAX_ABS_XFORM).any() or (np.abs(sc) > MAX_ABS_XFORM).any() \
            or (np.abs(sc) < MIN_ABS_SCALE).any():
        raise ValueError("input_offset / input_scale outside the app's accepted range")

    def mat(a):
        return [f32(row) for row in a]
    doc = {"feature_set": ref.FEATURE_SET, "model": name,
           "input_offset": f32(off), "input_scale": f32(sc),
           "dense1": {"weights": mat(q["w1"]), "bias": f32(q["b1"])},
           "dense2": {"weights": mat(q["w2"]), "bias": f32(q["b2"])},
           "dense3": {"weights": mat(q["w3"]), "bias": f32(q["b3"])}}
    validate_document(doc)
    return doc, clipped


def validate_document(doc):
    """Mirror of CandidateClassifier::parseWeights + validate (shapes, finiteness, bounds)."""
    if doc.get("feature_set") != ref.FEATURE_SET:
        raise ValueError("feature_set")
    shapes = {"dense1": (N_H1, N_IN), "dense2": (N_H2, N_H1), "dense3": (1, N_H2)}
    for key, (o, i) in shapes.items():
        w, b = doc[key]["weights"], doc[key]["bias"]
        if len(w) != o or any(len(r) != i for r in w) or len(b) != o:
            raise ValueError("%s has the wrong shape" % key)
        for val in [x for r in w for x in r] + list(b):
            if not math.isfinite(val) or abs(val) > MAX_ABS_WEIGHT:
                raise ValueError("%s holds a weight the app would reject" % key)
    for key in ("input_offset", "input_scale"):
        if len(doc[key]) != N_IN:
            raise ValueError(key + " has the wrong length")
    for o, s in zip(doc["input_offset"], doc["input_scale"]):
        if not (math.isfinite(o) and math.isfinite(s)) or abs(o) > MAX_ABS_XFORM \
                or abs(s) > MAX_ABS_XFORM or abs(s) < MIN_ABS_SCALE:
            raise ValueError("input_offset / input_scale the app would reject")


def _write_json(path, doc, indent=1):
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=indent, allow_nan=False)
        fh.write("\n")


def train(rows, name, seed=DEFAULTS["seed"], max_epochs=DEFAULTS["max_epochs"],
          patience=DEFAULTS["patience"], lr=DEFAULTS["lr"], l2=DEFAULTS["l2"],
          val_fraction=DEFAULTS["val_fraction"]):
    """Returns (weights_doc, meta). `rows` are dataset rows of ALL sessions;
    the held-out sessions are removed here."""
    sessions = sorted({r["session_id"] for r in rows})
    train_ids, held_ids = ds.split_sessions(sessions)
    fit_ids, val_ids = ds.split_sessions(train_ids, heldout_fraction=val_fraction,
                                         salt=ds.SPLIT_SALT + "/val")
    by = lambda ids: [r for r in rows if r["session_id"] in set(ids)]
    tr_rows, va_rows = by(fit_ids), by(val_ids)
    x_tr = np.array([r["features"] for r in tr_rows], dtype=np.float64)
    y_tr = np.array([r["label"] for r in tr_rows], dtype=np.float64)
    x_va = np.array([r["features"] for r in va_rows], dtype=np.float64)
    y_va = np.array([r["label"] for r in va_rows], dtype=np.float64)
    for tag, y in (("training", y_tr), ("validation", y_va)):
        if y.size == 0 or (y == 1).sum() == 0 or (y == 0).sum() == 0:
            raise ValueError("the %s side needs both labels (got %d rows); "
                             "more sessions or a different split are needed" % (tag, y.size))
    offset, scale = standardise(x_tr)
    norm = lambda x: (x - offset) * scale
    params, info = fit(norm(x_tr), y_tr, norm(x_va), y_va, seed, max_epochs, patience, lr, l2)
    doc, clipped = export_weights(params, offset, scale, name)
    meta = {"model": name, "seed": seed, "numpy_version": np.__version__,
            "python_version": sys.version.split()[0],
            "hyperparameters": {"max_epochs": max_epochs, "patience": patience, "lr": lr,
                                "l2": l2, "val_fraction": val_fraction, "optimizer": "adam",
                                "batch": "full", "class_weighting": "balanced"},
            "split": {"fit_sessions": fit_ids, "validation_sessions": val_ids,
                      "heldout_sessions_untouched": held_ids},
            "rows": {"fit": len(tr_rows), "validation": len(va_rows)},
            "weights_clipped": clipped, **info}
    return doc, meta


def write_parity(rows, weights_doc, path, n=16):
    """A small parity file (same format as tests/fixtures/classifier-parity-v1.json)
    so the C++ test can check the exported file against the Python reference."""
    cases = []
    step = max(1, len(rows) // n)
    for r in rows[::step][:n]:
        cases.append({"input": r["raw"], "expected": ref.score(weights_doc, r["raw"])})
    _write_json(path, {"feature_set": ref.FEATURE_SET, "tolerance": 1.0e-5, "cases": cases})


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dataset", required=True, help="dataset.jsonl from dataset.py")
    ap.add_argument("--name", required=True, help="model id written into the file")
    ap.add_argument("--out", required=True, help="weights JSON to write")
    ap.add_argument("--seed", type=int, default=DEFAULTS["seed"])
    ap.add_argument("--max-epochs", type=int, default=DEFAULTS["max_epochs"])
    ap.add_argument("--patience", type=int, default=DEFAULTS["patience"])
    ap.add_argument("--lr", type=float, default=DEFAULTS["lr"])
    ap.add_argument("--l2", type=float, default=DEFAULTS["l2"])
    args = ap.parse_args(argv)
    if args.name.startswith("untrained-"):
        print("refusing: the name 'untrained-*' is reserved for the placeholder model", file=sys.stderr)
        return 2
    rows = ds.read_rows(args.dataset)
    doc, meta = train(rows, args.name, args.seed, args.max_epochs, args.patience, args.lr, args.l2)
    meta["dataset_sha256"] = ds.sha256_file(args.dataset)
    _write_json(args.out, doc)
    from gate import sha256_file  # noqa: E402
    meta["weights_sha256"] = sha256_file(args.out)
    base = args.out[:-5] if args.out.endswith(".json") else args.out
    _write_json(base + ".meta.json", meta)
    write_parity(rows, doc, base + ".parity.json")
    print("trained %s: %d epochs (best %d, val loss %.4f), weights clipped %d"
          % (args.name, meta["epochs_run"], meta["best_epoch"], meta["best_val_loss"], meta["weights_clipped"]))
    print("wrote %s, %s.meta.json, %s.parity.json" % (args.out, base, base))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
