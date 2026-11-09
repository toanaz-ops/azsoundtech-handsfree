#!/usr/bin/env python3
"""Tests for the lane C1c offline pipeline: tools/nn/{dataset,evaluate,gate,
check_embedded_models,slim_log,train_mlp}.py.

Standard library only; the cases that train (numpy) skip themselves when numpy
is missing, so a CI runner without it still runs everything else.

    python tests/test_nn_pipeline.py
"""
import contextlib
import copy
import io
import json
import math
import os
import random
import shutil
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
NN = os.path.join(ROOT, "tools", "nn")
FIX = os.path.join(ROOT, "tests", "fixtures")
sys.path.insert(0, NN)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.dont_write_bytecode = True

import check_embedded_models  # noqa: E402
import dataset as ds  # noqa: E402
import evaluate  # noqa: E402
import gate  # noqa: E402
import logstats  # noqa: E402
import reference_forward as ref  # noqa: E402
import slim_log  # noqa: E402

try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False

SYN_DIR = os.path.join(NN, "models")
SYN_WEIGHTS = os.path.join(SYN_DIR, "synthetic-bootstrap-v0.json")
SYN_REPORT = os.path.join(SYN_DIR, "synthetic-bootstrap-v0.eval-report.json")
SYN_MANIFEST = os.path.join(SYN_DIR, "synthetic-bootstrap-v0.manifest.json")
SYN_LOGS = os.path.join(NN, "data", "synthetic-bootstrap-v0")
BIN_HZ = 23.4375


def read_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def write_json(path, doc):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh)


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="nn_pipeline_")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)


# ---------------------------------------------------------------------------
class DatasetFromFixture(TempDirCase):
    def test_rows_labels_drops_and_candidates(self):
        m = ds.build_dataset([os.path.join(FIX, "session-nn-dataset.jsonl")], self.tmp)
        rows = ds.read_rows(os.path.join(self.tmp, "dataset.jsonl"))
        self.assertEqual([(r["label"], round(r["raw"]["hz"], 1)) for r in rows],
                         [(1, 1007.8), (0, 2437.5), (1, 500.0)])
        self.assertEqual(m["dropped"], {"no_ctx": 1, "no_verdict": 1, "non_finite": 1, "origin_preset": 1})
        self.assertEqual(m["unparsable_lines"], 1)
        self.assertEqual((m["label1"], m["label0"], m["candidates"]), (2, 1, 1))
        self.assertEqual(m["sources"], {"real": 3})
        self.assertEqual(m["dataset_sha256"], ds.sha256_file(os.path.join(self.tmp, "dataset.jsonl")))
        # candidates are exported separately and unlabelled
        cands = ds.read_rows(os.path.join(self.tmp, "candidates.jsonl"))
        self.assertEqual(len(cands), 1)
        self.assertIsNone(cands[0]["label"])
        self.assertEqual(cands[0]["nn_score"], 0.427)
        # the dataset never contains a candidate
        self.assertTrue(all(r["label"] in (0, 1) for r in rows))

    def test_mono_row_has_no_other_lane_and_stereo_row_has(self):
        ds.build_dataset([os.path.join(FIX, "session-nn-dataset.jsonl")], self.tmp)
        rows = ds.read_rows(os.path.join(self.tmp, "dataset.jsonl"))
        self.assertFalse(rows[0]["raw"]["hasOtherLane"])
        self.assertEqual(rows[0]["raw"]["lrRatio"], 0.0)
        self.assertEqual(rows[0]["features"][7:9], [0.0, 0.0])
        self.assertTrue(rows[1]["raw"]["hasOtherLane"])
        self.assertAlmostEqual(rows[1]["raw"]["lrRatio"], 2.5)
        self.assertEqual(rows[1]["features"][7], 1.0)
        self.assertAlmostEqual(rows[1]["features"][8], math.log10(2.5))

    def test_features_come_from_the_shared_reference_code(self):
        ds.build_dataset([os.path.join(FIX, "session-nn-dataset.jsonl")], self.tmp)
        for r in ds.read_rows(os.path.join(self.tmp, "dataset.jsonl")):
            self.assertEqual(r["features"], ref.raw_features(r["raw"]))
            self.assertEqual(len(r["features"]), 16)

    def test_same_input_same_bytes(self):
        a, b = os.path.join(self.tmp, "a"), os.path.join(self.tmp, "b")
        ma = ds.build_dataset([os.path.join(FIX, "session-nn-dataset.jsonl")], a)
        mb = ds.build_dataset([os.path.join(FIX, "session-nn-dataset.jsonl")], b)
        self.assertEqual(ma["dataset_sha256"], mb["dataset_sha256"])

    def test_hand_written_fixtures_that_cannot_be_rebuilt_are_dropped_not_invented(self):
        # session-sample.jsonl is schema 1 (no rise_ratio); session-nn-shadow.jsonl has no ctx.
        m = ds.build_dataset([os.path.join(FIX, "session-sample.jsonl"),
                              os.path.join(FIX, "session-nn-shadow.jsonl")], self.tmp)
        self.assertEqual(m["rows"], 0)
        self.assertIn("missing_field:rise_ratio", m["dropped"])
        self.assertIn("no_ctx", m["dropped"])


class VerdictJoinEqualsLogstats(TempDirCase):
    def labelled_by_logstats(self, path):
        events, _ = ds.load_events(path)
        s = logstats.summarise(events)
        return [(round(n["hz"], 3), 1 if n["verdict"] == "good" else 0)
                for n in s["notches"] if n["origin"] == "detector" and n["verdict"] in ("good", "false")]

    def test_join_matches_logstats_on_every_fixture(self):
        for name in ("session-nn-dataset.jsonl", "session-sample.jsonl", "session-superseded.jsonl",
                     "session-nn-shadow.jsonl", "session-candidates.jsonl"):
            path = os.path.join(FIX, name)
            events, _ = ds.load_events(path)
            # count verdict joins directly, without the ctx requirement
            joined = 0
            summary = logstats.summarise(events)
            for n in summary["notches"]:
                if n["origin"] == "detector" and n["verdict"] in ("good", "false"):
                    joined += 1
            _, _, stats = ds.build_session(events, "x")
            labelled = stats["rows"] + sum(v for k, v in stats["dropped"].items() if k not in
                                           ("no_verdict",) and not k.startswith("origin_"))
            self.assertEqual(labelled, joined, name)

    def test_labelled_rows_equal_logstats_labels_on_the_fixture(self):
        path = os.path.join(FIX, "session-nn-dataset.jsonl")
        ds.build_dataset([path], self.tmp)
        rows = ds.read_rows(os.path.join(self.tmp, "dataset.jsonl"))
        theirs = self.labelled_by_logstats(path)
        mine = [(round(r["raw"]["hz"], 3), r["label"]) for r in rows]
        # logstats also counts the notches this builder had to drop (NaN, no ctx)
        for pair in mine:
            self.assertIn(pair, theirs)
        self.assertEqual(len(theirs) - len(mine), 2)      # E (NaN) and G (no ctx)

    def test_a_superseded_notch_keeps_no_verdict(self):
        events, _ = ds.load_events(os.path.join(FIX, "session-nn-dataset.jsonl"))
        _, _, stats = ds.build_session(events, "x")
        self.assertEqual(stats["dropped"]["no_verdict"], 1)    # notch C, closed by D's Set
        s = logstats.summarise(events)
        self.assertEqual(s["superseded"], 1)


# ---------------------------------------------------------------------------
def make_full_log_line(raw, bin_index, hz, now_len=1025, stereo=None):
    """A real-shaped notch_set line: ctx.now is the WHOLE spectrum."""
    now = [0.01 + 0.0001 * (i % 7) for i in range(now_len)]
    for k in range(11):
        i = bin_index + k - 5
        if 0 <= i < now_len:
            now[i] = raw["nearMags"][k]
    e = {"ev": "notch_set", "t": 1000.0, "slot": 0, "lane": 0, "index": 0, "hz": hz, "q": 30,
         "depth_db": -6, "origin": "detector", "confirmed_lane": 0, "score": 0.9,
         "peakiness": raw["peakiness"], "p_norm": raw["pNorm"], "rise": raw["rNorm"],
         "novelty": raw["mNorm"], "penalty": raw["penalty"], "asymmetry": 1, "persist_needed": 3,
         "thr": 10, "rise_ratio": raw["riseRatio"],
         "ctx": {"bins": now_len, "bin_hz": BIN_HZ, "now": now, "ref": now, "ref_age_ms": 100.0}}
    if raw["hasOtherLane"]:
        e["lr_ratio"] = raw["lrRatio"]
        e["ctx"]["other_lane_now"] = now
    return e


class RebuildFromSpectrum(TempDirCase):
    def raw(self, has_other=False):
        return {"hz": 0.0, "peakiness": 18.5, "pNorm": 0.8, "rNorm": 0.9, "riseRatio": 2.5,
                "mNorm": 0.7, "penalty": 1.0, "hasOtherLane": has_other,
                "lrRatio": 3.0 if has_other else 0.0,
                "nearMags": [0.02, 0.03, 0.05, 0.1, 0.4, 5.0, 0.5, 0.12, 0.06, 0.03, 0.02]}

    def write_log(self, events, name="session-t.jsonl"):
        p = os.path.join(self.tmp, name)
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(json.dumps({"ev": "session_start", "t": 0, "log_schema": 3}) + "\n")
            for e in events:
                fh.write(json.dumps(e) + "\n")
        return p

    def rows_of(self, events):
        p = self.write_log(events + [{"ev": "verdict", "t": 2000.0, "slot": 0, "lane": 0,
                                      "index": 0, "verdict": "good"}])
        out = os.path.join(self.tmp, "out")
        m = ds.build_dataset([p], out)
        return m, ds.read_rows(os.path.join(out, "dataset.jsonl"))

    def test_near_mags_are_the_eleven_bins_around_lround_hz_over_bin_hz(self):
        for b in (6, 43, 500, 1018):
            for has_other in (False, True):
                raw = self.raw(has_other)
                hz = (b + 0.31) * BIN_HZ            # sub-bin offset, as the analyser adds one
                raw["hz"] = hz
                e = make_full_log_line(raw, b, hz)
                m, rows = self.rows_of([e])
                self.assertEqual(len(rows), 1, (b, m["dropped"]))
                self.assertEqual(rows[0]["raw"]["nearMags"], raw["nearMags"])
                self.assertEqual(rows[0]["raw"]["hasOtherLane"], has_other)
                self.assertEqual(rows[0]["raw"]["hz"], hz)

    def test_off_spectrum_neighbours_are_zero_like_the_app(self):
        for b, expect in ((0, [0, 0, 0, 0, 0]), (2, [0, 0, 0]), (1024, [])):
            raw = self.raw()
            raw["nearMags"] = [0.5] * 11
            hz = b * BIN_HZ
            raw["hz"] = hz
            e = make_full_log_line(raw, b, hz)
            _, rows = self.rows_of([e])
            near = rows[0]["raw"]["nearMags"]
            # indices of near[] that fall outside 0..1024
            for k in range(11):
                i = b + k - 5
                if i < 0 or i >= 1025:
                    self.assertEqual(near[k], 0.0, (b, k))
                else:
                    self.assertEqual(near[k], 0.5, (b, k))

    def test_wrong_length_or_out_of_range_is_dropped_with_a_reason(self):
        raw = self.raw()
        hz = 43 * BIN_HZ
        raw["hz"] = hz
        good = make_full_log_line(raw, 43, hz)
        short = copy.deepcopy(good)
        short["ctx"]["now"] = short["ctx"]["now"][:500]
        far = copy.deepcopy(good)
        far["hz"] = 1.0e6
        nothing = copy.deepcopy(good)
        del nothing["ctx"]["now"]
        m, rows = self.rows_of([short])
        self.assertEqual((len(rows), m["dropped"]), (0, {"ctx_now_wrong_length": 1}))
        m, rows = self.rows_of([far])
        self.assertEqual((len(rows), m["dropped"]), (0, {"bin_outside_spectrum": 1}))
        m, rows = self.rows_of([nothing])
        self.assertEqual((len(rows), m["dropped"]), (0, {"no_ctx_now": 1}))

    def test_missing_and_non_numeric_fields_are_named(self):
        raw = self.raw()
        hz = 43 * BIN_HZ
        raw["hz"] = hz
        for key in ("peakiness", "p_norm", "rise", "novelty", "penalty", "rise_ratio"):
            e = make_full_log_line(raw, 43, hz)
            del e[key]
            m, rows = self.rows_of([e])
            self.assertEqual(m["dropped"], {"missing_field:" + key: 1}, key)
            e = make_full_log_line(raw, 43, hz)
            e[key] = "high"
            m, rows = self.rows_of([e])
            self.assertEqual(m["dropped"], {"non_numeric:" + key: 1}, key)

    def test_non_finite_values_are_dropped(self):
        raw = self.raw()
        hz = 43 * BIN_HZ
        raw["hz"] = hz
        for key in ("peakiness", "rise_ratio", "hz"):
            e = make_full_log_line(raw, 43, hz)
            e[key] = float("inf") if key != "hz" else float("nan")
            m, rows = self.rows_of([e])
            self.assertEqual(rows, [], key)
            self.assertEqual(sum(m["dropped"].values()), 1, key)
        e = make_full_log_line(raw, 43, hz)
        e["ctx"]["now"][43] = float("nan")
        m, rows = self.rows_of([e])
        self.assertEqual((rows, m["dropped"]), ([], {"non_finite": 1}))

    def test_slimmed_log_gives_identical_rows(self):
        raw = self.raw(True)
        events = []
        for i, b in enumerate((3, 43, 700, 1020)):
            r = copy.deepcopy(raw)
            hz = (b - 0.2) * BIN_HZ
            r["hz"] = hz
            e = make_full_log_line(r, b, hz)
            e["lane"] = 0
            e["index"] = i
            e["t"] = 1000.0 * (i + 1)
            events += [e, {"ev": "verdict", "t": 1000.0 * (i + 1) + 500, "slot": 0, "lane": 0,
                           "index": i, "verdict": "good" if i % 2 else "false"}]
        full = self.write_log(events, "session-full.jsonl")
        slim = os.path.join(self.tmp, "session-slim.jsonl")
        with contextlib.redirect_stdout(io.StringIO()):
            slim_log.main([full, slim])
        self.assertLess(os.path.getsize(slim), os.path.getsize(full) / 5)
        a = ds.build_dataset([full], os.path.join(self.tmp, "a"))
        b = ds.build_dataset([slim], os.path.join(self.tmp, "b"))
        ra = ds.read_rows(os.path.join(self.tmp, "a", "dataset.jsonl"))
        rb = ds.read_rows(os.path.join(self.tmp, "b", "dataset.jsonl"))
        self.assertEqual(a["rows"], 4)
        for x, y in zip(ra, rb):
            x["session_id"] = y["session_id"] = "s"
        self.assertEqual(ra, rb)

    def test_unknown_source_never_passes_as_real(self):
        raw = self.raw()
        hz = 43 * BIN_HZ
        raw["hz"] = hz
        p = os.path.join(self.tmp, "session-x.jsonl")
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(json.dumps({"ev": "session_start", "t": 0, "source": "reel"}) + "\n")
            fh.write(json.dumps(make_full_log_line(raw, 43, hz)) + "\n")
            fh.write(json.dumps({"ev": "verdict", "t": 2000.0, "slot": 0, "lane": 0, "index": 0,
                                 "verdict": "good"}) + "\n")
        m = ds.build_dataset([p], os.path.join(self.tmp, "o"))
        self.assertEqual(m["sources"], {"unknown": 1})


class FeaturesMatchTheCppParityFixture(unittest.TestCase):
    """The stored parity fixture holds scores the C++ test reproduces to 1e-5.
    Recomputing them from ds-style features with an INDEPENDENT forward pass pins
    the Python feature table to the C++ one."""

    def test_python_features_reproduce_the_cpp_pinned_scores(self):
        w = read_json(os.path.join(FIX, "classifier-fixture-v1.json"))
        parity = read_json(os.path.join(FIX, "classifier-parity-v1.json"))
        tol = parity["tolerance"]

        def dense(x, layer):
            return [sum(a * b for a, b in zip(row, x)) + bias
                    for row, bias in zip(layer["weights"], layer["bias"])]
        worst, n = 0.0, 0
        for case in parity["cases"]:
            f = ref.raw_features(case["input"])
            if case["expected"] is None:
                self.assertIsNone(f)
                continue
            x = [(f[i] - w["input_offset"][i]) * w["input_scale"][i] for i in range(16)]
            h1 = [math.tanh(v) for v in dense(x, w["dense1"])]
            h2 = [math.tanh(v) for v in dense(h1, w["dense2"])]
            z = dense(h2, w["dense3"])[0]
            y = 1.0 / (1.0 + math.exp(-z))
            worst = max(worst, abs(y - case["expected"]))
            n += 1
        self.assertGreaterEqual(n, 64)
        self.assertLessEqual(worst, tol)


# ---------------------------------------------------------------------------
class SplitBySession(unittest.TestCase):
    def test_no_session_on_both_sides_and_deterministic(self):
        rnd = random.Random(5)
        for n in range(2, 40):
            ids = ["session-%d-%d" % (n, rnd.randrange(10 ** 6)) for _ in range(n)]
            tr, he = ds.split_sessions(ids)
            self.assertEqual(set(tr) & set(he), set())
            self.assertEqual(set(tr) | set(he), set(ids))
            self.assertGreaterEqual(len(he), 1)
            self.assertGreaterEqual(len(tr), 1)
            self.assertEqual((tr, he), ds.split_sessions(list(reversed(ids))))

    def test_fewer_than_two_sessions_is_an_error(self):
        with self.assertRaises(ValueError):
            ds.split_sessions(["only"])

    def test_evaluate_uses_the_same_split(self):
        w = read_json(SYN_WEIGHTS)
        rows = []
        for s in range(6):
            for i in range(6):
                rows.append({"session_id": "s%d" % s, "source": "real", "label": i % 2,
                             "score": 0.5 + 0.01 * i, "raw": {
                                 "hz": 1000.0, "peakiness": 10.0 + i, "pNorm": 0.5, "rNorm": 0.5,
                                 "riseRatio": 1.5, "mNorm": 0.5, "penalty": 1.0, "hasOtherLane": False,
                                 "lrRatio": 0.0, "nearMags": [0.1] * 5 + [1.0] + [0.1] * 5}})
        rep = evaluate.evaluate(rows, w, SYN_WEIGHTS, "0" * 64)
        tr, he = ds.split_sessions([r["session_id"] for r in rows])
        self.assertEqual(rep["split"]["train_sessions"], tr)
        self.assertEqual(rep["split"]["heldout_sessions"], he)
        self.assertEqual(rep["heldout"]["rows"], 6 * len(he))


class AucAndOperatingPoint(unittest.TestCase):
    def test_hand_computed_cases(self):
        self.assertAlmostEqual(evaluate.roc_auc([0.1, 0.4, 0.35, 0.8], [0, 0, 1, 1]), 0.75)
        self.assertEqual(evaluate.roc_auc([0.1, 0.2, 0.8, 0.9], [0, 0, 1, 1]), 1.0)
        self.assertEqual(evaluate.roc_auc([0.9, 0.8, 0.2, 0.1], [0, 0, 1, 1]), 0.0)

    def test_ties_count_half(self):
        self.assertEqual(evaluate.roc_auc([0.5, 0.5], [0, 1]), 0.5)
        self.assertEqual(evaluate.roc_auc([0.3, 0.3, 0.3, 0.3], [0, 1, 0, 1]), 0.5)
        # pos {0.5, 0.9}, neg {0.5, 0.1}: pairs (0.5>0.5 tie, 0.5>0.1, 0.9>0.5, 0.9>0.1) = (0.5+1+1+1)/4
        self.assertAlmostEqual(evaluate.roc_auc([0.5, 0.9, 0.5, 0.1], [1, 1, 0, 0]), 0.875)

    def test_single_class_or_bad_scores_are_undefined_never_a_number(self):
        self.assertIsNone(evaluate.roc_auc([0.1, 0.9], [1, 1]))
        self.assertIsNone(evaluate.roc_auc([0.1, 0.9], [0, 0]))
        self.assertIsNone(evaluate.roc_auc([], []))
        self.assertIsNone(evaluate.roc_auc([0.1, float("nan")], [0, 1]))
        self.assertIsNone(evaluate.roc_auc([0.1, None], [0, 1]))

    def test_reject_at_retention(self):
        pos = [0.9, 0.8, 0.7, 0.6, 0.5]
        neg = [0.1, 0.65, 0.3]
        t, kept, rej = evaluate.reject_at_retention(pos + neg, [1] * 5 + [0] * 3, 0.8)
        self.assertEqual((t, kept), (0.6, 0.8))
        self.assertAlmostEqual(rej, 2.0 / 3.0)
        # 95 % of 20 positives is exactly 19 rows, not 20 by float error
        pos20 = [0.5 + 0.01 * i for i in range(20)]
        t, kept, _ = evaluate.reject_at_retention(pos20 + [0.0], [1] * 20 + [0], 0.95)
        self.assertGreaterEqual(kept, 0.95)
        self.assertEqual(t, pos20[1])
        self.assertIsNone(evaluate.reject_at_retention([0.1, 0.2], [1, 1], 0.95))

    def test_evaluate_reports_undefined_auc_for_a_one_label_heldout(self):
        w = read_json(SYN_WEIGHTS)
        rows = []
        for s in range(5):
            for i in range(4):
                rows.append({"session_id": "s%d" % s, "source": "real", "label": 1, "score": 0.7,
                             "raw": {"hz": 1000.0, "peakiness": 10.0, "pNorm": 0.5, "rNorm": 0.5,
                                     "riseRatio": 1.5, "mNorm": 0.5, "penalty": 1.0,
                                     "hasOtherLane": False, "lrRatio": 0.0,
                                     "nearMags": [0.1] * 5 + [1.0] + [0.1] * 5}})
        rep = evaluate.evaluate(rows, w, SYN_WEIGHTS, "0" * 64)
        self.assertIsNone(rep["heldout"]["auc_model"])
        self.assertIsNone(rep["heldout"]["reject_label0_at_retention"])
        json.dumps(rep, allow_nan=False)           # no NaN leaks into the report


# ---------------------------------------------------------------------------
class GateFailsClosed(TempDirCase):
    def setUp(self):
        super().setUp()
        self.weights = os.path.join(self.tmp, "w.json")
        write_json(self.weights, {"feature_set": 1, "model": "candidate-x"})
        self.report = {
            "rows_total": 400, "sessions_total": 5, "source_composition": {"real": 400},
            "dataset_sha256": "d" * 64, "weights_sha256": gate.sha256_file(self.weights),
            "heldout": {"rows": 100, "label1": 55, "label0": 45, "auc_model": 0.90,
                        "auc_baseline": 0.70, "retention_target": 0.95, "retention_achieved": 0.96,
                        "reject_label0_at_retention": 0.5}}

    def fails(self, report=None, manifest=None):
        return gate.check(self.report if report is None else report, self.weights, manifest)

    def assertFailsWith(self, fragment, report):
        f = self.fails(report)
        self.assertTrue(f, "gate passed but should have failed on: " + fragment)
        self.assertTrue(any(fragment in x for x in f), (fragment, f))

    def test_a_good_report_passes(self):
        self.assertEqual(self.fails(), [])

    def test_synthetic_source_never_passes(self):
        r = copy.deepcopy(self.report)
        r["source_composition"] = {"synthetic": 400}
        self.assertFailsWith("not all real", r)
        r["source_composition"] = {"real": 300, "synthetic": 100}
        self.assertFailsWith("not all real", r)
        r["source_composition"] = {"real": 400, "unknown": 1}
        self.assertFailsWith("not all real", r)

    def test_too_few_rows(self):
        r = copy.deepcopy(self.report)
        r["rows_total"] = 299
        r["source_composition"] = {"real": 299}
        self.assertFailsWith("only 299 labelled rows", r)

    def test_too_few_sessions(self):
        r = copy.deepcopy(self.report)
        r["sessions_total"] = 2
        self.assertFailsWith("only 2 sessions", r)

    def test_heldout_too_small_or_single_label(self):
        r = copy.deepcopy(self.report)
        r["heldout"].update(rows=59, label1=30, label0=29)
        self.assertFailsWith("held-out has 59 rows", r)
        r = copy.deepcopy(self.report)
        r["heldout"].update(rows=100, label1=100, label0=0)
        self.assertFailsWith("label0", r)
        r["heldout"].update(label1=0, label0=100)
        self.assertFailsWith("label1", r)

    def test_auc_below_floor(self):
        r = copy.deepcopy(self.report)
        r["heldout"].update(auc_model=0.79, auc_baseline=0.5)
        self.assertFailsWith("< 0.80", r)

    def test_auc_not_above_baseline_plus_margin(self):
        r = copy.deepcopy(self.report)
        r["heldout"].update(auc_model=0.85, auc_baseline=0.82)
        self.assertFailsWith("baseline score AUC", r)
        r["heldout"].update(auc_model=0.88, auc_baseline=0.83)       # exactly the margin: passes
        self.assertEqual(self.fails(r), [])

    def test_rejection_below_floor(self):
        r = copy.deepcopy(self.report)
        r["heldout"]["reject_label0_at_retention"] = 0.29
        self.assertFailsWith("label-0 rows rejected", r)

    def test_retention_below_target(self):
        r = copy.deepcopy(self.report)
        r["heldout"]["retention_target"] = 0.90
        self.assertFailsWith("retention", r)
        r = copy.deepcopy(self.report)
        r["heldout"]["retention_achieved"] = 0.94
        self.assertFailsWith("achieved retention", r)

    def test_weights_hash_mismatch(self):
        r = copy.deepcopy(self.report)
        r["weights_sha256"] = "0" * 64
        self.assertFailsWith("sha256 mismatch", r)
        with open(self.weights, "a", encoding="utf-8") as fh:        # weights edited after the report
            fh.write(" ")
        self.assertFailsWith("sha256 mismatch", self.report)

    def test_line_endings_do_not_change_the_weights_hash(self):
        with open(self.weights, "rb") as fh:
            lf = fh.read()
        crlf = os.path.join(self.tmp, "w-crlf.json")
        with open(crlf, "wb") as fh:
            fh.write(lf.replace(b"{", b"{" + bytes([13, 10]), 1))
        with open(self.weights, "wb") as fh:
            fh.write(lf.replace(b"{", b"{" + bytes([10]), 1))
        self.assertEqual(gate.sha256_file(crlf), gate.sha256_file(self.weights))

    def test_missing_weights_file(self):
        f = gate.check(self.report, os.path.join(self.tmp, "nope.json"))
        self.assertTrue(any("cannot read weights" in x for x in f))

    def test_dataset_hash_must_match_the_manifest(self):
        self.assertEqual(self.fails(manifest={"dataset_sha256": "d" * 64, "sources": {"real": 400}}), [])
        f = self.fails(manifest={"dataset_sha256": "e" * 64})
        self.assertTrue(any("manifest" in x for x in f))
        f = self.fails(manifest={"dataset_sha256": "d" * 64, "sources": {"real": 300, "synthetic": 100}})
        self.assertTrue(any("non-real" in x for x in f))

    def test_removing_any_single_field_fails(self):
        def leaves(d, path=()):
            for k, v in d.items():
                if isinstance(v, dict):
                    yield from leaves(v, path + (k,))
                else:
                    yield path + (k,)
        for path in list(leaves(self.report)):
            r = copy.deepcopy(self.report)
            cur = r
            for k in path[:-1]:
                cur = cur[k]
            del cur[path[-1]]
            if path == ("dataset_sha256",):        # only consulted when a manifest is given
                self.assertEqual(self.fails(r), [])
                self.assertTrue(gate.check(r, self.weights, {"dataset_sha256": "d" * 64}))
                continue
            self.assertTrue(self.fails(r), "gate passed without " + ".".join(path))

    def test_nan_inf_bool_and_string_metrics_fail(self):
        for bad in (float("nan"), float("inf"), -float("inf"), True, "0.9", None):
            for key in ("auc_model", "auc_baseline", "reject_label0_at_retention",
                        "retention_achieved", "rows", "label1", "label0"):
                r = copy.deepcopy(self.report)
                r["heldout"][key] = bad
                self.assertTrue(self.fails(r), "passed with %s=%r" % (key, bad))

    def test_report_that_is_not_an_object_or_unreadable_fails(self):
        self.assertTrue(gate.check([], self.weights))
        self.assertTrue(gate.check(None, self.weights))
        self.assertTrue(gate.check_files(os.path.join(self.tmp, "missing.json"), self.weights))
        p = os.path.join(self.tmp, "bad.json")
        with open(p, "w", encoding="utf-8") as fh:
            fh.write("{not json")
        self.assertTrue(gate.check_files(p, self.weights))

    def test_nan_token_in_a_report_file_fails(self):
        p = os.path.join(self.tmp, "rep.json")
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(self.report).replace("0.9,", "NaN,", 1))
        self.assertTrue(gate.check_files(p, self.weights))

    def test_cli_exit_codes(self):
        rp = os.path.join(self.tmp, "r.json")
        write_json(rp, self.report)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.main(["--report", rp, "--weights", self.weights]), 0)
        bad = copy.deepcopy(self.report)
        bad["source_composition"] = {"synthetic": 400}
        write_json(rp, bad)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.main(["--report", rp, "--weights", self.weights]), 1)


class CommittedSyntheticBootstrap(TempDirCase):
    """The committed synthetic model + report: pipeline artefacts, and the proof
    that a synthetic model cannot pass."""

    def test_report_belongs_to_the_weights_and_the_dataset_rebuilds(self):
        report = read_json(SYN_REPORT)
        self.assertEqual(report["weights_sha256"], gate.sha256_file(SYN_WEIGHTS))
        self.assertEqual(report["source_composition"], {"synthetic": report["rows_total"]})
        logs = sorted(os.path.join(SYN_LOGS, n) for n in os.listdir(SYN_LOGS) if n.endswith(".jsonl"))
        self.assertGreaterEqual(len(logs), 6)
        m = ds.build_dataset(logs, self.tmp)
        self.assertEqual(m["dataset_sha256"], report["dataset_sha256"])
        self.assertEqual(m["dataset_sha256"], read_json(SYN_MANIFEST)["dataset_sha256"])
        self.assertGreaterEqual(m["rows"], 300)
        self.assertGreater(m["label1"], 50)
        self.assertGreater(m["label0"], 50)
        self.assertEqual(m["sources"], {"synthetic": m["rows"]})

    def test_the_gate_rejects_it_for_being_synthetic(self):
        fails = gate.check_files(SYN_REPORT, SYN_WEIGHTS, SYN_MANIFEST)
        self.assertTrue(any("not all real" in f for f in fails), fails)
        # even with the evidence "fixed up" so it looks real, the manifest still says synthetic
        rep = read_json(SYN_REPORT)
        rep["source_composition"] = {"real": rep["rows_total"]}
        f = gate.check(rep, SYN_WEIGHTS, read_json(SYN_MANIFEST))
        self.assertTrue(any("non-real" in x for x in f), f)

    def test_synthetic_logs_say_so_in_session_start(self):
        for n in os.listdir(SYN_LOGS):
            with open(os.path.join(SYN_LOGS, n), "r", encoding="utf-8") as fh:
                head = json.loads(fh.readline())
            self.assertEqual(head["ev"], "session_start")
            self.assertEqual(head["source"], "synthetic")
            self.assertEqual(head["log_schema"], 3)

    def test_total_size_of_committed_logs_stays_small(self):
        total = sum(os.path.getsize(os.path.join(SYN_LOGS, n)) for n in os.listdir(SYN_LOGS))
        self.assertLess(total, 1_000_000)


class ExportedWeights(unittest.TestCase):
    """What the exporter writes must be what CandidateClassifier::parseWeights accepts."""

    def check_like_the_cpp_parser(self, w):
        self.assertEqual(w["feature_set"], 1)
        shapes = {"dense1": (16, 16), "dense2": (8, 16), "dense3": (1, 8)}
        for k, (o, i) in shapes.items():
            self.assertEqual(len(w[k]["weights"]), o)
            self.assertTrue(all(len(r) == i for r in w[k]["weights"]))
            self.assertEqual(len(w[k]["bias"]), o)
            for v in [x for r in w[k]["weights"] for x in r] + w[k]["bias"]:
                self.assertTrue(math.isfinite(v) and abs(v) <= 1000.0)
        self.assertEqual(len(w["input_offset"]), 16)
        self.assertEqual(len(w["input_scale"]), 16)
        for o, s in zip(w["input_offset"], w["input_scale"]):
            self.assertTrue(math.isfinite(o) and abs(o) <= 1e6)
            self.assertTrue(math.isfinite(s) and 1e-6 <= abs(s) <= 1e6)

    def test_committed_model_respects_the_cpp_limits_and_the_reference_loader_reads_it(self):
        w = ref.load_weights(SYN_WEIGHTS)
        self.check_like_the_cpp_parser(w)
        self.assertEqual(w["model"], "synthetic-bootstrap-v0")
        self.assertFalse(w["model"].startswith("untrained-"))

    def test_committed_parity_file_matches_the_reference(self):
        w = ref.load_weights(SYN_WEIGHTS)
        p = read_json(os.path.join(SYN_DIR, "synthetic-bootstrap-v0.parity.json"))
        self.assertGreaterEqual(len(p["cases"]), 8)
        for c in p["cases"]:
            self.assertAlmostEqual(ref.score(w, c["input"]), c["expected"], delta=1e-12)


# ---------------------------------------------------------------------------
class EmbeddedModelGate(TempDirCase):
    def test_the_models_the_app_embeds_today_pass(self):
        self.assertEqual(check_embedded_models.check_models_dir(os.path.join(ROOT, "models")), [])

    def test_untrained_placeholder_is_allowed_without_a_report(self):
        shutil.copy(os.path.join(ROOT, "models", "candidate-classifier-untrained-v1.json"), self.tmp)
        self.assertEqual(check_embedded_models.check_models_dir(self.tmp), [])

    def test_a_trained_model_without_a_report_fails(self):
        shutil.copy(SYN_WEIGHTS, self.tmp)
        problems = check_embedded_models.check_models_dir(self.tmp)
        self.assertEqual(len(problems), 1)
        self.assertIn("no synthetic-bootstrap-v0.eval-report.json", problems[0])

    def test_the_synthetic_report_does_not_get_it_through(self):
        for f in (SYN_WEIGHTS, SYN_REPORT, SYN_MANIFEST):
            shutil.copy(f, self.tmp)
        problems = check_embedded_models.check_models_dir(self.tmp)
        self.assertTrue(problems)
        self.assertTrue(any("not all real" in p for p in problems), problems)

    def test_a_good_looking_report_for_other_weights_does_not_get_it_through(self):
        # a report that would pass the gate, but belongs to a different weights file
        w = os.path.join(self.tmp, "candidate-real-v1.json")
        doc = read_json(SYN_WEIGHTS)
        doc["model"] = "candidate-real-v1"
        write_json(w, doc)
        rep = {"rows_total": 400, "sessions_total": 5, "source_composition": {"real": 400},
               "weights_sha256": "a" * 64,
               "heldout": {"rows": 100, "label1": 55, "label0": 45, "auc_model": 0.9,
                           "auc_baseline": 0.7, "retention_target": 0.95,
                           "retention_achieved": 0.96, "reject_label0_at_retention": 0.5}}
        write_json(os.path.join(self.tmp, "candidate-real-v1.eval-report.json"), rep)
        problems = check_embedded_models.check_models_dir(self.tmp)
        self.assertTrue(any("sha256 mismatch" in p for p in problems), problems)
        rep["weights_sha256"] = gate.sha256_file(w)          # now it belongs: and passes
        write_json(os.path.join(self.tmp, "candidate-real-v1.eval-report.json"), rep)
        self.assertEqual(check_embedded_models.check_models_dir(self.tmp), [])

    def test_empty_or_missing_directory_fails(self):
        self.assertTrue(check_embedded_models.check_models_dir(self.tmp))
        self.assertTrue(check_embedded_models.check_models_dir(os.path.join(self.tmp, "nope")))


# ---------------------------------------------------------------------------
def toy_rows(n_sessions=8, per_session=30, seed=3):
    rnd = random.Random(seed)
    rows = []
    for s in range(n_sessions):
        for i in range(per_session):
            label = i % 2
            pk = rnd.uniform(25.0, 60.0) if label else rnd.uniform(10.0, 16.0)
            centre = 10.0 ** rnd.uniform(-2.0, 0.0)
            skirt = centre * (0.02 if label else 0.4)
            raw = {"hz": 2.0 ** rnd.uniform(7.0, 13.0), "peakiness": pk,
                   "pNorm": rnd.uniform(0.6, 1.0) if label else rnd.uniform(0.1, 0.5),
                   "rNorm": rnd.random(), "riseRatio": 2.0 ** rnd.uniform(-1, 3), "mNorm": rnd.random(),
                   "penalty": 1.0, "hasOtherLane": False, "lrRatio": 0.0,
                   "nearMags": [skirt] * 3 + [skirt, skirt, centre, skirt, skirt] + [skirt] * 3}
            rows.append({"session_id": "toy-%d" % s, "source": "real", "label": label,
                         "raw": raw, "features": ref.raw_features(raw), "score": rnd.random()})
    return rows


@unittest.skipUnless(HAVE_NUMPY, "numpy not installed: training tests skipped")
class TrainingWithNumpy(TempDirCase):
    def test_separable_toy_data_reaches_high_auc_and_exports_valid_weights(self):
        import train_mlp
        rows = toy_rows()
        doc, meta = train_mlp.train(rows, "toy-v1", max_epochs=300)
        train_mlp.validate_document(doc)
        ExportedWeights().check_like_the_cpp_parser(doc)
        sessions = {r["session_id"] for r in rows}
        _, held = ds.split_sessions(sessions)
        heldrows = [r for r in rows if r["session_id"] in held]
        scores = [ref.score(doc, r["raw"]) for r in heldrows]
        auc = evaluate.roc_auc(scores, [r["label"] for r in heldrows])
        self.assertGreater(auc, 0.95, auc)
        self.assertEqual(set(meta["split"]["heldout_sessions_untouched"]), set(held))
        self.assertFalse(set(meta["split"]["fit_sessions"]) & set(held))
        self.assertFalse(set(meta["split"]["validation_sessions"]) & set(held))
        self.assertFalse(set(meta["split"]["fit_sessions"]) & set(meta["split"]["validation_sessions"]))

    def test_same_seed_is_bit_identical_and_a_different_seed_is_not(self):
        import train_mlp
        rows = toy_rows()
        a, _ = train_mlp.train(rows, "toy-v1", seed=11, max_epochs=60)
        b, _ = train_mlp.train(rows, "toy-v1", seed=11, max_epochs=60)
        c, _ = train_mlp.train(rows, "toy-v1", seed=12, max_epochs=60)
        self.assertEqual(json.dumps(a, sort_keys=True), json.dumps(b, sort_keys=True))
        self.assertNotEqual(json.dumps(a, sort_keys=True), json.dumps(c, sort_keys=True))

    def test_the_exporter_refuses_what_the_app_would_reject(self):
        import numpy as np
        import train_mlp
        p = train_mlp.init_params(np.random.default_rng(1))
        off, sc = np.zeros(16), np.ones(16)
        p["w1"][0, 0] = float("nan")
        with self.assertRaises(ValueError):
            train_mlp.export_weights(p, off, sc, "x")
        p = train_mlp.init_params(np.random.default_rng(1))
        bad = sc.copy()
        bad[3] = 0.0                                   # a dead input
        with self.assertRaises(ValueError):
            train_mlp.export_weights(p, off, bad, "x")
        bad[3] = 2.0e6
        with self.assertRaises(ValueError):
            train_mlp.export_weights(p, off, bad, "x")
        p["w2"][1, 1] = 5000.0                         # clipped, counted, still valid
        doc, clipped = train_mlp.export_weights(p, off, sc, "x")
        self.assertEqual(clipped, 1)
        train_mlp.validate_document(doc)

    def test_constant_feature_gets_a_scale_the_app_accepts(self):
        import numpy as np
        import train_mlp
        x = np.zeros((10, 16))
        x[:, 1] = np.arange(10)
        off, sc = train_mlp.standardise(x)
        self.assertTrue(np.all(np.abs(sc) >= 1e-6))
        self.assertEqual(sc[0], 1.0)                   # constant column: neutral scale

    def test_one_label_on_a_side_is_an_error_not_a_silent_model(self):
        import train_mlp
        rows = toy_rows()
        for r in rows:
            r["label"] = 1
        with self.assertRaises(ValueError):
            train_mlp.train(rows, "toy-v1", max_epochs=5)

    def test_the_reserved_placeholder_name_is_refused(self):
        import train_mlp
        p = os.path.join(self.tmp, "d.jsonl")
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            for r in toy_rows():
                fh.write(json.dumps(r) + "\n")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(train_mlp.main(["--dataset", p, "--name", "untrained-x",
                                             "--out", os.path.join(self.tmp, "w.json")]), 2)


if __name__ == "__main__":
    unittest.main(verbosity=1)
