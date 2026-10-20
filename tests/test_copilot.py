#!/usr/bin/env python3
"""Lane L: pins every copilot finding rule against hand-built logs.

    python tests/test_copilot.py

Stdlib only; never touches the network. The --ask tools are exercised only
when the `anthropic` package is importable (they need its decorator), and
even then no request is sent.
"""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "copilot"))
import copilot  # noqa: E402
import logstats  # noqa: E402  (copilot put tools/ on the path)

FIXTURE = ROOT / "tests" / "fixtures" / "session-sample.jsonl"


def start(**kw) -> dict:
    return {"ev": "session_start", "t": 0, "device": "Fake", "sample_rate": 48000, **kw}


def nset(t, hz, slot=0, lane=0, index=0, depth=-6, origin="detector") -> dict:
    return {"ev": "notch_set", "t": t, "slot": slot, "lane": lane, "index": index,
            "hz": hz, "depth_db": depth, "origin": origin}


def nclear(t, slot=0, lane=0, index=0, reason="auto_release") -> dict:
    return {"ev": "notch_clear", "t": t, "slot": slot, "lane": lane, "index": index,
            "reason": reason}


def verdict(t, v, slot=0, lane=0, index=0) -> dict:
    return {"ev": "verdict", "t": t, "slot": slot, "lane": lane, "index": index, "verdict": v}


def codes(events) -> list[str]:
    return [f["code"] for f in copilot.findings(events)]


class Recurrence(unittest.TestCase):
    def test_twice_is_info_three_times_is_warn(self):
        ev = [start(), nset(1000, 1000.0), nclear(2000), nset(3000, 1010.0), nclear(4000)]
        f = [x for x in copilot.findings(ev) if x["code"] == "recurrence"]
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0]["severity"], "info")
        ev += [nset(5000, 1005.0), nclear(6000)]
        f = [x for x in copilot.findings(ev) if x["code"] == "recurrence"]
        self.assertEqual(f[0]["severity"], "warn")
        self.assertEqual(f[0]["evidence"]["count"], copilot.RECUR_WARN)

    def test_different_frequencies_do_not_recur(self):
        ev = [start(), nset(1000, 1000.0), nclear(2000), nset(3000, 2000.0), nclear(4000)]
        self.assertNotIn("recurrence", codes(ev))


class Ceiling(unittest.TestCase):
    def tuning(self, depth, slot=-1):
        return {"ev": "tuning", "t": 1, "slot": slot, "depth_db": depth}

    def test_retune_to_ceiling_alerts(self):
        ev = [start(), self.tuning(-18), nset(1000, 800.0, depth=-6),
              {"ev": "notch_retune", "t": 1300, "slot": 0, "lane": 0, "index": 0, "depth_db": -12},
              {"ev": "notch_retune", "t": 1600, "slot": 0, "lane": 0, "index": 0, "depth_db": -18}]
        f = [x for x in copilot.findings(ev) if x["code"] == "ceiling"]
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0]["severity"], "alert")
        self.assertEqual(f[0]["evidence"]["deepest_db"], -18)

    def test_shallower_than_ceiling_is_quiet(self):
        ev = [start(), self.tuning(-18), nset(1000, 800.0, depth=-12)]
        self.assertNotIn("ceiling", codes(ev))

    def test_per_slot_ceiling_wins_over_global(self):
        ev = [start(), self.tuning(-24), self.tuning(-12, slot=1),
              nset(1000, 800.0, slot=1, depth=-12)]
        self.assertIn("ceiling", codes(ev))

    def test_false_verdict_and_preset_are_not_loop_gain(self):
        ev = [start(), self.tuning(-18), nset(1000, 800.0, depth=-18), verdict(1500, "false"),
              nset(2000, 400.0, index=1, depth=-18, origin="preset")]
        self.assertNotIn("ceiling", codes(ev))

    def test_no_tuning_event_no_claim(self):
        ev = [start(), nset(1000, 800.0, depth=-30)]
        self.assertNotIn("ceiling", codes(ev))


class Stacked(unittest.TestCase):
    def test_close_and_alive_together(self):
        # 1000 and 1100 Hz: 0.137 octave apart, both open.
        ev = [start(), nset(1000, 1000.0, index=0), nset(1500, 1100.0, index=1)]
        f = [x for x in copilot.findings(ev) if x["code"] == "stacked"]
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0]["evidence"]["hz"], [1000.0, 1100.0])

    def test_not_stacked_when_first_already_cleared(self):
        ev = [start(), nset(1000, 1000.0, index=0), nclear(1200, index=0),
              nset(1500, 1100.0, index=1)]
        self.assertNotIn("stacked", codes(ev))

    def test_not_stacked_across_lanes_or_far_apart(self):
        ev = [start(), nset(1000, 1000.0, lane=0), nset(1500, 1100.0, lane=1, index=1),
              nset(1600, 1500.0, lane=0, index=2)]
        self.assertNotIn("stacked", codes(ev))


class Burst(unittest.TestCase):
    def test_four_in_a_minute(self):
        ev = [start()] + [nset(1000 + i * 10_000, 500.0 + 300 * i, index=i) for i in range(4)]
        f = [x for x in copilot.findings(ev) if x["code"] == "burst"]
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0]["evidence"]["count"], 4)
        self.assertEqual(f[0]["severity"], "alert")

    def test_spread_out_is_quiet(self):
        ev = [start()] + [nset(1000 + i * 30_000, 500.0 + 300 * i, index=i) for i in range(4)]
        self.assertNotIn("burst", codes(ev))

    def test_preset_and_soundcheck_notches_do_not_count(self):
        ev = [start()] + [nset(1000 + i * 1000, 500.0 + 300 * i, index=i, origin="preset")
                          for i in range(5)]
        self.assertNotIn("burst", codes(ev))


class Labels(unittest.TestCase):
    def test_false_rate_needs_enough_verdicts(self):
        ev = [start(), nset(1000, 500.0), verdict(1100, "false"),
              nset(2000, 900.0, index=1), verdict(2100, "good", index=1)]
        self.assertNotIn("false_rate", codes(ev))     # 2 judged < FALSE_MIN_JUDGED
        ev += [nset(3000, 1500.0, index=2), verdict(3100, "good", index=2)]
        self.assertIn("false_rate", codes(ev))        # 1/3 >= 0.3

    def test_unjudged(self):
        ev = [start()] + [nset(1000 + i * 30_000, 500.0 + 300 * i, index=i) for i in range(3)]
        self.assertIn("unjudged", codes(ev))

    def test_dropped(self):
        ev = [start(), {"ev": "session_end", "t": 10, "dropped_events": 7}]
        f = copilot.findings(ev)
        self.assertEqual([x["code"] for x in f], ["dropped"])


class Contract(unittest.TestCase):
    def test_empty_session_has_no_findings(self):
        self.assertEqual(copilot.findings([start()]), [])

    def test_sorted_most_severe_first(self):
        ev = logstats.load(FIXTURE)
        sev = [copilot.SEVERITY_ORDER[f["severity"]] for f in copilot.findings(ev)]
        self.assertEqual(sev, sorted(sev))

    def test_fixture(self):
        # session-sample: 1007.8 Hz placed twice, second at -18 = the -18 ceiling
        # (good verdict); 2437.5 at -18 is FALSE so it is not a ceiling hit;
        # 1 false of 3 judged.
        self.assertEqual(sorted(codes(logstats.load(FIXTURE))),
                         ["ceiling", "false_rate", "recurrence"])

    def test_advice_never_says_raise_gain(self):
        # Every rule, fired at once. Spec §4.2: nothing advises more gain or
        # a deeper ceiling.
        ev = ([start(), {"ev": "tuning", "t": 1, "slot": -1, "depth_db": -12}]
              + [nset(1000 + i * 5000, 1000.0 + 50 * i, index=i, depth=-12) for i in range(4)]
              + [verdict(30_000, "false", index=0), verdict(30_001, "good", index=1),
                 verdict(30_002, "good", index=2),
                 {"ev": "session_end", "t": 40_000, "dropped_events": 1}])
        fs = copilot.findings(ev)
        self.assertGreaterEqual(len({f["code"] for f in fs}), 5)
        for f in fs:
            # "đừng tăng trần" is the warning AGAINST it, so it is removed first.
            text = (f["title"] + " " + f["advice"]).lower().replace("đừng tăng trần", "")
            for bad in ("tăng gain", "tăng độ lợi", "tắt chống hú", "tăng trần"):
                self.assertNotIn(bad, text, f["code"])

    def test_cli_json_round_trips(self):
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = copilot.main([str(FIXTURE), "--json"])
        self.assertEqual(rc, 0)
        self.assertEqual(len(json.loads(buf.getvalue())), 3)


try:
    import anthropic  # noqa: F401
    HAVE_SDK = True
except ImportError:
    HAVE_SDK = False


@unittest.skipUnless(HAVE_SDK, "anthropic not installed")
class AskTools(unittest.TestCase):
    """The --ask tools run locally; this calls them directly, no request is sent."""

    def setUp(self):
        self.tools = {t.name: t for t in copilot.make_tools(logstats.load(FIXTURE))}

    def test_tool_set_is_read_only_and_named(self):
        self.assertEqual(sorted(self.tools),
                         ["get_findings", "list_notches", "notch_context", "session_summary"])

    def test_tools_return_json(self):
        summary = json.loads(self.tools["session_summary"].call({}))
        self.assertEqual(summary["device"], "Fake ASIO")
        self.assertEqual(len(json.loads(self.tools["get_findings"].call({}))), 3)
        rows = json.loads(self.tools["list_notches"].call({"slot": 0}))
        self.assertEqual(rows[0]["number"], 1)
        ctx = json.loads(self.tools["notch_context"].call({"number": 1}))
        self.assertAlmostEqual(ctx["hz"], 1007.8)
        self.assertIn("ctx", ctx)
        self.assertIn("error", json.loads(self.tools["notch_context"].call({"number": 99})))


if __name__ == "__main__":
    unittest.main(verbosity=1)
