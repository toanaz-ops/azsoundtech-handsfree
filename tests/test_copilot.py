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


class SidecarCli(unittest.TestCase):
    """The frozen sidecar is read by the app as UTF-8 JSON on a pipe."""

    SCRIPT = ROOT / "tools" / "copilot" / "copilot.py"

    def run_cli(self, *extra: str) -> bytes:
        import os
        import subprocess
        env = {k: v for k, v in os.environ.items()
               if k not in ("PYTHONIOENCODING", "PYTHONUTF8")}
        r = subprocess.run([sys.executable, str(self.SCRIPT), str(FIXTURE), "--json", *extra],
                           capture_output=True, env=env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))
        return r.stdout

    def test_piped_stdout_is_utf8_and_matches_findings(self):
        out = self.run_cli()
        parsed = json.loads(out.decode("utf-8"))
        self.assertEqual(parsed, copilot.findings(logstats.load(FIXTURE)))
        # Vietnamese must arrive as real UTF-8 text, not escaped or code-paged.
        self.assertTrue(any(ord(c) > 127 for c in out.decode("utf-8")))

    def test_missing_file_exits_2(self):
        self.assertEqual(copilot.main([str(ROOT / "no-such-session.jsonl")]), 2)

    def test_low_priority_does_not_change_output(self):
        self.assertEqual(self.run_cli("--low-priority"), self.run_cli())


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


# --- L2b: providers, credentials, safety filter ------------------------------

import base64  # noqa: E402
import contextlib  # noqa: E402
import io  # noqa: E402
import os  # noqa: E402
import tempfile  # noqa: E402
import threading  # noqa: E402
import time  # noqa: E402
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer  # noqa: E402
from unittest import mock  # noqa: E402

SECRET = "sk-test-SECRET-key-123456"
WINDOWS = sys.platform == "win32"


def answer_body(text, usage=True) -> dict:
    d = {"choices": [{"finish_reason": "stop", "message": {"role": "assistant", "content": text}}]}
    if usage:
        d["usage"] = {"prompt_tokens": 11, "completion_tokens": 7}
    return d


def tool_body(name, args, call_id="c1") -> dict:
    return {"choices": [{"finish_reason": "tool_calls", "message": {
        "role": "assistant", "content": None,
        "tool_calls": [{"id": call_id, "type": "function",
                        "function": {"name": name, "arguments": args}}]}}]}


class FakeServer:
    """A local Chat Completions server. `script` is consumed one entry per
    request: (status, body-dict | bytes | callable(headers)), (302, location)
    or ("hang", seconds)."""

    def __init__(self, script):
        self.script = list(script)
        self.requests: list[dict] = []
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                raw = self.rfile.read(n)
                try:
                    body = json.loads(raw)
                except ValueError:
                    body = None
                outer.requests.append({"path": self.path, "auth": self.headers.get("Authorization"),
                                       "body": body})
                if not outer.script:
                    self.send_response(500)
                    self.end_headers()
                    return
                status, payload = outer.script.pop(0)
                if status == "hang":
                    time.sleep(payload)
                    return
                if status in (301, 302):
                    self.send_response(status)
                    self.send_header("Location", payload)
                    self.end_headers()
                    return
                if callable(payload):
                    payload = payload(self.headers)
                data = payload if isinstance(payload, bytes) else json.dumps(payload).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.httpd.server_address[1]}/v1"

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class CliHarness(unittest.TestCase):
    def setUp(self):
        self.servers: list[FakeServer] = []
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.addCleanup(lambda: [s.close() for s in self.servers])

    def server(self, script) -> FakeServer:
        s = FakeServer(script)
        self.servers.append(s)
        return s

    def run_cli(self, argv, env=None, stdin=""):
        extra = {copilot.CREDENTIALS_DIR_ENV: self.tmp.name}
        extra.update(env or {})
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.dict(os.environ, extra), \
                mock.patch.object(sys, "stdin", io.StringIO(stdin)), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            if "OPENAI_API_KEY" not in extra:
                os.environ.pop("OPENAI_API_KEY", None)
            try:
                rc = copilot.main(argv)
            except SystemExit as exc:
                rc = exc.code
        return rc, out.getvalue(), err.getvalue()

    def ask(self, srv, *extra, env=None, question="Vì sao hú?"):
        return self.run_cli([str(FIXTURE), "--ask", question, "--provider", "openai_compatible",
                             "--base-url", srv.url, "--model", "m", *extra], env=env)


class OpenAiAdapter(CliHarness):
    def test_two_step_tool_round_trip(self):
        srv = self.server([(200, tool_body("get_findings", "{}")),
                           (200, answer_body("Cùng một tần số hú lại."))])
        rc, out, err = self.ask(srv)
        self.assertEqual(rc, 0, err)
        self.assertIn("Cùng một tần số hú lại.", out)
        self.assertEqual(len(srv.requests), 2)
        first = srv.requests[0]
        self.assertEqual(first["path"], "/v1/chat/completions")
        self.assertEqual(first["body"]["messages"][0]["role"], "system")
        self.assertEqual(first["body"]["tool_choice"], "auto")
        self.assertEqual(sorted(t["function"]["name"] for t in first["body"]["tools"]),
                         sorted(copilot.TOOL_SPECS))
        tool_msgs = [m for m in srv.requests[1]["body"]["messages"] if m["role"] == "tool"]
        self.assertEqual(len(tool_msgs), 1)
        self.assertEqual(tool_msgs[0]["tool_call_id"], "c1")
        self.assertEqual(len(json.loads(tool_msgs[0]["content"])), 3)

    def test_answer_without_tool_call(self):
        srv = self.server([(200, answer_body("Không có gì bất thường."))])
        rc, out, _ = self.ask(srv)
        self.assertEqual(rc, 0)
        self.assertTrue(out.strip().endswith("Không có gì bất thường."))
        self.assertEqual(len(srv.requests), 1)

    def test_http_401_and_500(self):
        for status in (401, 500):
            srv = self.server([(status, {"error": "nope"})])
            rc, out, err = self.ask(srv)
            self.assertEqual(rc, 3)
            self.assertIn(str(status), err)

    def test_error_body_excerpt_is_capped(self):
        srv = self.server([(500, b"x" * 5000)])
        rc, _, err = self.ask(srv)
        self.assertEqual(rc, 3)
        self.assertLess(len(err), 400, err)

    def test_non_json_body(self):
        srv = self.server([(200, b"<html>not json</html>")])
        rc, _, err = self.ask(srv)
        self.assertEqual(rc, 3)
        self.assertIn("JSON", err)

    def test_missing_choices(self):
        srv = self.server([(200, {"id": "x"})])
        self.assertEqual(self.ask(srv)[0], 3)

    def test_empty_content(self):
        srv = self.server([(200, answer_body("   "))])
        self.assertEqual(self.ask(srv)[0], 3)

    def test_timeout(self):
        srv = self.server([("hang", 3)])
        t0 = time.monotonic()
        rc, _, err = self.ask(srv, "--timeout", "0.5")
        self.assertEqual(rc, 3)
        self.assertLess(time.monotonic() - t0, 2.5)
        self.assertIn("Quá hạn", err)

    def test_connection_refused(self):
        srv = self.server([])
        url = srv.url
        srv.close()
        self.servers.remove(srv)
        rc, _, _ = self.run_cli([str(FIXTURE), "--ask", "q", "--provider", "openai_compatible",
                                 "--base-url", url, "--model", "m", "--timeout", "2"])
        self.assertEqual(rc, 3)

    def test_invalid_json_arguments_do_not_crash(self):
        srv = self.server([(200, tool_body("list_notches", "{not json")),
                           (200, answer_body("Xong."))])
        rc, _, err = self.ask(srv)
        self.assertEqual(rc, 0, err)
        msg = [m for m in srv.requests[1]["body"]["messages"] if m["role"] == "tool"][0]
        self.assertIn("error", json.loads(msg["content"]))

    def test_unknown_tool_does_not_crash(self):
        srv = self.server([(200, tool_body("delete_everything", "{}")),
                           (200, answer_body("Xong."))])
        rc, _, err = self.ask(srv)
        self.assertEqual(rc, 0, err)
        msg = [m for m in srv.requests[1]["body"]["messages"] if m["role"] == "tool"][0]
        self.assertIn("unknown tool", json.loads(msg["content"])["error"])

    def test_tool_loop_exhausted(self):
        srv = self.server([(200, tool_body("get_findings", "{}", f"c{i}")) for i in range(13)])
        rc, _, err = self.ask(srv)
        self.assertEqual(rc, 3)
        self.assertIn("tool loop exhausted", err)
        self.assertEqual(len(srv.requests), copilot.MAX_ITERATIONS)

    def test_content_filter_is_refusal(self):
        body = answer_body("x")
        body["choices"][0]["finish_reason"] = "content_filter"
        srv = self.server([(200, body)])
        self.assertEqual(self.ask(srv)[0], 3)

    def test_redirect_is_error_and_key_does_not_follow(self):
        target = self.server([(200, answer_body("leaked"))])
        srv = self.server([(302, target.url + "/steal")])
        rc, out, err = self.ask(srv, env={"OPENAI_API_KEY": SECRET})
        self.assertEqual(rc, 3)
        self.assertEqual(target.requests, [])
        self.assertNotIn("leaked", out)
        self.assertEqual(srv.requests[0]["auth"], "Bearer " + SECRET)

    def test_authorization_header_present_only_with_key(self):
        srv = self.server([(200, answer_body("a"))])
        self.ask(srv, env={"OPENAI_API_KEY": SECRET})
        self.assertEqual(srv.requests[0]["auth"], "Bearer " + SECRET)
        srv = self.server([(200, answer_body("a"))])
        self.ask(srv)
        self.assertIsNone(srv.requests[0]["auth"])

    def test_key_never_in_output_on_failure(self):
        def echo(headers):
            return {"error": "bad key " + headers.get("Authorization", "")}
        srv = self.server([(401, echo)])
        rc, out, err = self.ask(srv, env={"OPENAI_API_KEY": SECRET})
        self.assertEqual(rc, 3)
        self.assertNotIn(SECRET, out + err)
        self.assertIn("401", err)

    def test_json_output_shape(self):
        srv = self.server([(200, answer_body("Trả lời."))])
        rc, out, err = self.ask(srv, "--json")
        self.assertEqual(rc, 0, err)
        doc = json.loads(out)
        self.assertEqual(sorted(doc), ["answer", "blocked", "findings", "model", "provider", "usage"])
        self.assertIs(doc["blocked"], False)   # L2b contract C1
        self.assertEqual((doc["answer"], doc["provider"], doc["model"]),
                         ("Trả lời.", "openai_compatible", "m"))
        self.assertEqual(doc["usage"], {"input_tokens": 11, "output_tokens": 7})
        self.assertEqual(len(doc["findings"]), 3)

    def test_usage_null_when_server_omits_it(self):
        srv = self.server([(200, answer_body("a", usage=False))])
        doc = json.loads(self.ask(srv, "--json")[1])
        self.assertEqual(doc["usage"], {"input_tokens": None, "output_tokens": None})

    def test_text_output_prints_findings_then_answer(self):
        srv = self.server([(200, answer_body("Câu trả lời."))])
        _, out, _ = self.ask(srv)
        self.assertLess(out.index("hú lại"), out.index("Câu trả lời."))

    def test_unsafe_answer_is_blocked_exit_4(self):
        srv = self.server([(200, answer_body("bạn nên tăng gain monitor"))])
        rc, out, _ = self.ask(srv)
        self.assertEqual(rc, 4)
        self.assertNotIn("tăng gain", out)
        self.assertIn(copilot.BLOCKED_MESSAGE, out)
        srv = self.server([(200, answer_body("bạn nên tăng gain monitor"))])
        rc, out, _ = self.ask(srv, "--json")
        self.assertEqual(rc, 4)
        self.assertNotIn("tăng gain", out)


class SafetyFilter(unittest.TestCase):
    def test_blocks_vietnamese(self):
        for t in ("Hãy tăng gain monitor", "TĂNG ĐỘ LỢI lên", "tắt chống hú đi", "Có thể tăng trần lên -24"):
            self.assertTrue(copilot.violates_safety(t), t)

    def test_blocks_english(self):
        for t in ("You should Raise the Gain", "increase gain on ch 3", "Turn off feedback suppression",
                  "disable feedback now", "raise the ceiling to -24 dB"):
            self.assertTrue(copilot.violates_safety(t), t)

    def test_allows_warnings_against(self):
        for t in ("đừng tăng trần", "Không nên tăng gain", "Đừng tắt chống hú", "không tăng trần",
                  "đừng tăng gain monitor", "Don't raise the gain", "never turn off feedback",
                  "Do not increase gain", "NEVER Disable Feedback", "you should not raise the ceiling",
                  "Hạ gain monitor 3 dB và thử ở âm lượng nhỏ."):
            self.assertFalse(copilot.violates_safety(t), t)

    def test_mixed_still_blocks(self):
        self.assertTrue(copilot.violates_safety("đừng tăng gain nhưng hãy tắt chống hú"))
        self.assertTrue(copilot.violates_safety("Do not raise the gain; instead increase gain."))

    def test_curly_apostrophe(self):
        self.assertFalse(copilot.violates_safety("Don’t raise the gain"))


class SharedTools(unittest.TestCase):
    def test_schema_table_matches_functions_and_runs(self):
        funcs = copilot.make_tool_functions(logstats.load(FIXTURE))
        self.assertEqual(sorted(funcs), sorted(copilot.TOOL_SPECS))
        self.assertEqual(len(json.loads(copilot.run_tool(funcs, "get_findings", "{}"))), 3)
        self.assertEqual(json.loads(copilot.run_tool(funcs, "notch_context", '{"number": "1"}'))["slot"], 0)
        self.assertIn("error", json.loads(copilot.run_tool(funcs, "notch_context", "{}")))
        self.assertIn("error", json.loads(copilot.run_tool(funcs, "get_findings", '{"x": 1}')))
        self.assertIn("error", json.loads(copilot.run_tool(funcs, "nope", "{}")))


class CliArguments(CliHarness):
    def bad(self, argv):
        rc, _, err = self.run_cli(argv)
        self.assertEqual(rc, copilot.EXIT_BAD_ARGS, err)

    def test_openai_needs_model_and_base_url(self):
        self.bad([str(FIXTURE), "--ask", "q", "--provider", "openai_compatible", "--base-url", "http://x/v1"])
        self.bad([str(FIXTURE), "--ask", "q", "--provider", "openai_compatible", "--model", "m"])

    def test_base_url_rejected_for_anthropic(self):
        self.bad([str(FIXTURE), "--ask", "q", "--base-url", "http://x/v1"])

    def test_non_http_base_url_rejected(self):
        for url in ("ftp://x/v1", "file:///etc/passwd", "localhost:11434"):
            self.bad([str(FIXTURE), "--ask", "q", "--provider", "openai_compatible",
                      "--base-url", url, "--model", "m"])

    def test_unknown_provider_and_bad_timeout(self):
        self.bad([str(FIXTURE), "--ask", "q", "--provider", "nope"])
        self.bad([str(FIXTURE), "--ask", "q", "--timeout", "0"])

    def test_ask_needs_a_file(self):
        self.bad(["--ask", "q"])

    def test_key_flags_are_exclusive_with_ask(self):
        self.bad([str(FIXTURE), "--ask", "q", "--has-key"])

    def test_empty_stdin_key_rejected(self):
        rc, _, _ = self.run_cli(["--store-key", "--provider", "anthropic"], stdin="\n")
        self.assertEqual(rc, copilot.EXIT_BAD_ARGS)

    def test_exit_2_still_means_unreadable_log(self):
        rc, _, _ = self.run_cli([str(ROOT / "no-such.jsonl")])
        self.assertEqual(rc, 2)


@unittest.skipUnless(WINDOWS, "DPAPI is Windows only")
class Credentials(CliHarness):
    def path(self) -> Path:
        return Path(self.tmp.name) / copilot.CREDENTIALS_FILE

    def test_round_trip_store_has_resolve_delete(self):
        self.assertEqual(self.run_cli(["--has-key", "--provider", "anthropic"])[0], 1)
        rc, out, err = self.run_cli(["--store-key", "--provider", "anthropic"], stdin=SECRET + "\n")
        self.assertEqual(rc, 0, err)
        self.assertNotIn(SECRET, out + err)
        raw = self.path().read_bytes()
        self.assertNotIn(SECRET.encode(), raw)
        self.assertNotIn(base64.b64encode(SECRET.encode()), raw)
        doc = json.loads(raw.decode("utf-8"))
        self.assertEqual(doc["version"], 1)
        self.assertEqual(list(doc["profiles"]), ["anthropic"])
        self.assertEqual(self.run_cli(["--has-key", "--provider", "anthropic"])[0], 0)
        self.assertEqual(self.run_cli(["--has-key", "--provider", "openai_compatible",
                                       "--base-url", "http://localhost:1/v1"])[0], 1)
        with mock.patch.dict(os.environ, {copilot.CREDENTIALS_DIR_ENV: self.tmp.name}):
            self.assertEqual(copilot.resolve_key("anthropic"), SECRET)
        rc, out, err = self.run_cli(["--delete-key", "--provider", "anthropic"])
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.run_cli(["--has-key", "--provider", "anthropic"])[0], 1)

    def store_openai(self, srv, key=SECRET):
        rc, _, err = self.run_cli(["--store-key", "--provider", "openai_compatible",
                                   "--base-url", srv.url], stdin=key + "\n")
        self.assertEqual(rc, 0, err)

    def test_stored_key_is_sent_as_bearer(self):
        srv = self.server([(200, answer_body("a"))])
        self.store_openai(srv)
        self.assertEqual(self.ask(srv)[0], 0)
        self.assertEqual(srv.requests[0]["auth"], "Bearer " + SECRET)

    def test_stored_key_wins_over_env(self):
        srv = self.server([(200, answer_body("a"))])
        self.store_openai(srv)
        self.ask(srv, env={"OPENAI_API_KEY": "from-env"})
        self.assertEqual(srv.requests[0]["auth"], "Bearer " + SECRET)

    def test_corrupt_blob_is_no_key_with_one_line_note(self):
        self.path().write_text(json.dumps({"version": 1, "profiles": {
            "openai_compatible": {"blob": base64.b64encode(b"garbage").decode()}}}), encoding="utf-8")
        srv = self.server([(200, answer_body("a"))])
        rc, out, err = self.run_cli(["--has-key", "--provider", "openai_compatible",
                                     "--base-url", srv.url])
        self.assertEqual(rc, 1)
        self.assertEqual(len(err.strip().splitlines()), 1)
        self.assertEqual(self.ask(srv)[0], 0)
        self.assertIsNone(srv.requests[0]["auth"])

    def test_garbage_file_is_no_key(self):
        self.path().write_text("{not json", encoding="utf-8")
        self.assertEqual(self.run_cli(["--has-key", "--provider", "anthropic"])[0], 1)

    def test_deleting_one_profile_keeps_the_other(self):
        self.run_cli(["--store-key", "--provider", "anthropic"], stdin="k1\n")
        srv = self.server([])
        self.store_openai(srv, "k2")
        self.run_cli(["--delete-key", "--provider", "anthropic"])
        self.assertEqual(self.run_cli(["--has-key", "--provider", "openai_compatible",
                                       "--base-url", srv.url])[0], 0)

    def test_stored_key_never_in_output_on_failure(self):
        def echo(headers):
            return {"error": "bad key " + headers.get("Authorization", "")}
        srv = self.server([(401, echo)])
        self.store_openai(srv)
        rc, out, err = self.ask(srv)
        self.assertEqual(rc, 3)
        self.assertNotIn(SECRET, out + err)


@unittest.skipIf(WINDOWS, "non-Windows behaviour")
class CredentialsOffWindows(CliHarness):
    def test_store_raises_clear_error(self):
        with self.assertRaises(copilot.CredentialError):
            copilot.store_key("anthropic", "k")


class AnthropicKwargs(unittest.TestCase):
    def test_fallback_models_get_betas_and_fallbacks(self):
        for model in ("claude-opus-5-5", "claude-fable-5-1", "claude-opus-5", "claude-sonnet-5-5"):
            kw = copilot.build_anthropic_kwargs(model, [], "q")
            self.assertEqual(kw["betas"], ["server-side-fallback-2026-07-01"], model)
            self.assertEqual(kw["fallbacks"], "default", model)

    def test_other_models_get_neither(self):
        kw = copilot.build_anthropic_kwargs("claude-haiku-5-5", [], "q")
        self.assertNotIn("betas", kw)
        self.assertNotIn("fallbacks", kw)

    def test_call_shape_and_no_forbidden_params(self):
        kw = copilot.build_anthropic_kwargs("claude-opus-5-5", ["t"], "hỏi")
        self.assertEqual((kw["max_tokens"], kw["max_iterations"]), (16000, 12))
        self.assertEqual(kw["thinking"], {"type": "adaptive"})
        self.assertEqual(kw["output_config"], {"effort": "medium"})
        self.assertEqual(kw["messages"], [{"role": "user", "content": "hỏi"}])
        for banned in ("temperature", "top_p", "tool_choice"):
            self.assertNotIn(banned, kw)
        self.assertNotIn("budget_tokens", json.dumps(kw["thinking"]))


# --- L2b fix pass: exit codes, tool robustness, safety table, hardening ------

class ExitCodeDiscipline(CliHarness):
    def test_invalid_utf8_log_exits_2(self):
        bad = Path(self.tmp.name) / "bad.jsonl"
        bad.write_bytes(b'{"ev":"session_start","t":0}\n\xff\xfe not utf8\n')
        rc, out, err = self.run_cli([str(bad)])
        self.assertEqual(rc, 2)
        self.assertNotIn("Traceback", err)

    def test_non_object_json_line_exits_2(self):
        bad = Path(self.tmp.name) / "arr.jsonl"
        bad.write_text('{"ev":"session_start","t":0}\n[1,2,3]\n42\n', encoding="utf-8")
        rc, out, err = self.run_cli([str(bad)])
        self.assertEqual(rc, 2)
        self.assertNotIn("Traceback", err)

    def test_store_key_with_invalid_utf8_stdin_exits_3(self):
        stdin = io.TextIOWrapper(io.BytesIO(b"\xff\xfe\xfd\n"), encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.dict(os.environ, {copilot.CREDENTIALS_DIR_ENV: self.tmp.name}), \
                mock.patch.object(sys, "stdin", stdin), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = copilot.main(["--store-key", "--provider", "anthropic"])
        self.assertEqual(rc, 3)
        self.assertNotIn("Traceback", err.getvalue())

    def test_unexpected_exception_is_exit_3_type_name_only(self):
        with mock.patch.object(copilot, "load_key", side_effect=RuntimeError("TOPSECRET-detail")):
            rc, out, err = self.run_cli(["--has-key", "--provider", "anthropic"])
        self.assertEqual(rc, 3)
        self.assertIn("RuntimeError", err)
        self.assertNotIn("TOPSECRET", out + err)
        self.assertNotIn("Traceback", err)
        self.assertEqual(len(err.strip().splitlines()), 1)

    def test_exit_1_only_for_has_key_without_key(self):
        self.assertEqual(self.run_cli(["--has-key", "--provider", "openai_compatible",
                                       "--base-url", "http://localhost:1/v1"])[0], 1)
        for argv in ([str(ROOT / "no-such.jsonl")], [str(FIXTURE), "--ask", "q", "--timeout", "0"]):
            self.assertNotEqual(self.run_cli(argv)[0], 1)

    def test_system_exit_is_not_swallowed(self):
        rc, _, _ = self.run_cli(["--bogus-flag"])
        self.assertEqual(rc, copilot.EXIT_BAD_ARGS)


class AskJsonContract(CliHarness):
    def test_exit_3_prints_error_object_without_key(self):
        def echo(headers):
            return {"error": "bad key " + headers.get("Authorization", "")}
        srv = self.server([(401, echo)])
        rc, out, err = self.ask(srv, "--json", env={"OPENAI_API_KEY": SECRET})
        self.assertEqual(rc, 3)
        doc = json.loads(out)
        self.assertEqual(list(doc), ["error"])
        self.assertIn("401", doc["error"])
        self.assertNotIn(SECRET, out + err)
        self.assertEqual(err.strip(), doc["error"])   # same one-line reason on stderr

    def test_exit_3_without_json_prints_nothing_to_stdout(self):
        srv = self.server([(500, {"error": "x"})])
        rc, out, err = self.ask(srv)
        self.assertEqual(rc, 3)
        self.assertNotIn('"error"', out)

    def test_blocked_json_object(self):
        srv = self.server([(200, answer_body("bạn nên tăng gain monitor"))])
        rc, out, _ = self.ask(srv, "--json")
        self.assertEqual(rc, 4)
        self.assertEqual(json.loads(out), {"blocked": True, "message": copilot.BLOCKED_MESSAGE})
        self.assertNotIn("tăng gain", out)

    def test_unexpected_exception_json_has_type_name_only(self):
        srv = self.server([(200, answer_body("ok"))])
        with mock.patch.object(copilot, "violates_safety", side_effect=ValueError("PRIVATE-text")):
            rc, out, err = self.ask(srv, "--json")
        self.assertEqual(rc, 3)
        doc = json.loads(out)
        self.assertEqual(list(doc), ["error"])
        self.assertIn("ValueError", doc["error"])
        self.assertNotIn("PRIVATE", out + err)
        self.assertNotIn("Traceback", err)

    def test_exit_2_and_5_leave_stdout_empty(self):
        self.assertEqual(self.run_cli([str(ROOT / "no.jsonl"), "--ask", "q", "--json"])[1], "")
        rc, out, _ = self.run_cli([str(FIXTURE), "--ask", "q", "--json", "--timeout", "0"])
        self.assertEqual((rc, out), (5, ""))


class ToolRobustness(unittest.TestCase):
    def setUp(self):
        self.funcs = copilot.make_tool_functions(logstats.load(FIXTURE))

    def test_overflow_arguments_come_back_as_error_json(self):
        for name, raw in (("list_notches", '{"slot": Infinity}'),
                          ("notch_context", '{"number": 1e999}'),
                          ("notch_context", '{"number": -Infinity}')):
            self.assertIn("error", json.loads(copilot.run_tool(self.funcs, name, raw)))

    def test_exception_in_tool_is_error_json(self):
        def boom():
            raise RuntimeError("inner detail")
        out = json.loads(copilot.run_tool({"get_findings": boom}, "get_findings", "{}"))
        self.assertIn("RuntimeError", out["error"])
        self.assertNotIn("inner detail", out["error"])

    def test_deeply_nested_arguments_do_not_abort(self):
        self.assertIn("error", json.loads(copilot.run_tool(self.funcs, "get_findings", "[" * 100000)))

    def test_loop_continues_after_overflow_call(self):
        srv = FakeServer([(200, tool_body("list_notches", '{"slot": Infinity}')),
                          (200, answer_body("Xong."))])
        try:
            res = copilot.run_openai_compatible(logstats.load(FIXTURE), "q", "m", srv.url, None, 10)
        finally:
            srv.close()
        self.assertEqual(res["answer"], "Xong.")


class ToolResultCap(unittest.TestCase):
    def test_oversized_list_is_truncated_to_valid_json(self):
        rows = [{"i": i, "pad": "é" * 200} for i in range(5000)]
        funcs = {"list_notches": lambda slot=-1: json.dumps(rows, ensure_ascii=False)}
        out = copilot.run_tool(funcs, "list_notches", "{}")
        self.assertLessEqual(len(out.encode("utf-8")), copilot.MAX_TOOL_RESULT_BYTES)
        doc = json.loads(out)
        self.assertIs(doc["truncated"], True)
        self.assertEqual(doc["total"], 5000)
        self.assertEqual(doc["items"], rows[:len(doc["items"])])
        self.assertGreater(len(doc["items"]), 0)

    def test_small_result_is_untouched(self):
        funcs = {"get_findings": lambda: "[1, 2]"}
        self.assertEqual(copilot.run_tool(funcs, "get_findings", "{}"), "[1, 2]")


class ResponseCap(CliHarness):
    def test_body_over_4_mib_is_an_ask_error(self):
        big = b'{"pad": "' + b"a" * (copilot.MAX_RESPONSE_BYTES + 10) + b'"}'
        srv = self.server([(200, big)])
        rc, out, err = self.ask(srv)
        self.assertEqual(rc, 3)
        self.assertIn("4 MiB", err)


class PlaintextKeyGuard(CliHarness):
    URL = "http://example.invalid:9/v1"

    def argv(self):
        return [str(FIXTURE), "--ask", "q", "--provider", "openai_compatible",
                "--base-url", self.URL, "--model", "m"]

    def test_key_over_plain_http_to_non_loopback_is_refused_before_io(self):
        with mock.patch.object(copilot, "_post_json", side_effect=AssertionError("network used")):
            rc, out, err = self.run_cli(self.argv(), env={"OPENAI_API_KEY": SECRET})
        self.assertEqual(rc, 3)
        self.assertIn("http://", err)
        self.assertNotIn(SECRET, out + err)

    def test_no_key_plain_http_non_loopback_is_allowed(self):
        with mock.patch.object(copilot, "_post_json", return_value=answer_body("ok")):
            rc, out, err = self.run_cli(self.argv())
        self.assertEqual(rc, 0, err)

    def test_https_with_key_is_unchanged(self):
        argv = self.argv()
        argv[argv.index(self.URL)] = "https://example.invalid/v1"
        with mock.patch.object(copilot, "_post_json", return_value=answer_body("ok")):
            rc, _, err = self.run_cli(argv, env={"OPENAI_API_KEY": SECRET})
        self.assertEqual(rc, 0, err)

    def test_loopback_table(self):
        for host in ("localhost", "127.0.0.1", "127.9.9.9", "::1", "::ffff:127.0.0.1"):
            self.assertTrue(copilot._is_loopback(host), host)
        for host in ("example.com", "10.0.0.5", "192.168.1.2", "0.0.0.0", "", None, "127.0.0.1.evil.com"):
            self.assertFalse(copilot._is_loopback(host), host)


class RedactEnv(CliHarness):
    def test_env_keys_are_redacted_even_without_a_passed_key(self):
        with mock.patch.dict(os.environ, {"ANTHROPIC_API_KEY": "env-key-AAA-111",
                                          "ANTHROPIC_AUTH_TOKEN": "env-tok-BBB-222"}):
            out = copilot.redact("err env-key-AAA-111 and env-tok-BBB-222 end", None)
        self.assertEqual(out, "err *** and *** end")

    @unittest.skipUnless(HAVE_SDK, "anthropic not installed")
    def test_sdk_error_echoing_env_key_is_redacted(self):
        dummy = "env-dummy-key-ZZZ-999"

        def echo(headers):
            return {"type": "error", "error": {"type": "invalid_request_error",
                                               "message": "bad " + (headers.get("x-api-key") or "")}}
        srv = self.server([(400, echo)])
        base = srv.url[:-len("/v1")]
        rc, out, err = self.run_cli([str(FIXTURE), "--ask", "q", "--json"],
                                    env={"ANTHROPIC_API_KEY": dummy, "ANTHROPIC_BASE_URL": base})
        self.assertEqual(rc, 3, err)
        self.assertNotIn(dummy, out + err)
        self.assertIn("400", err)


@unittest.skipUnless(WINDOWS, "DPAPI is Windows only")
class CorruptStoreIsKept(CliHarness):
    def test_corrupt_file_is_moved_aside_not_lost(self):
        path = Path(self.tmp.name) / copilot.CREDENTIALS_FILE
        corrupt = path.with_name(path.name + ".corrupt")
        path.write_text("{not json at all", encoding="utf-8")
        rc, out, err = self.run_cli(["--store-key", "--provider", "anthropic"], stdin="k1\n")
        self.assertEqual(rc, 0, err)
        self.assertEqual(corrupt.read_text(encoding="utf-8"), "{not json at all")
        self.assertEqual(len(err.strip().splitlines()), 1)
        self.assertEqual(self.run_cli(["--has-key", "--provider", "anthropic"])[0], 0)
        # a second corrupt file replaces the older .corrupt; nothing is deleted
        path.write_text("[again]", encoding="utf-8")
        self.assertEqual(self.run_cli(["--store-key", "--provider", "anthropic"], stdin="k2\n")[0], 0)
        self.assertEqual(corrupt.read_text(encoding="utf-8"), "[again]")


class SafetyTable(unittest.TestCase):
    BLOCK = ("Tang gain", "tang am luong", "tăng âm lượng lên", "đẩy fader lên", "turn it up",
             "boost the mic", "+3 dB", "nâng gain", "increase the gain", "tăng​gain",
             "tăng​ gain", "cộng 3 dB", "thêm 3 dB", "turn the gain up", "vặn volume lên",
             "mở to mic", "bring up the level", "crank the master", "fader up", "gain lên",
             "không giảm gain, tăng gain", "Không, tăng gain", "Hãy tăng gain monitor", "hãy mở to micro lên", "tăng micro lên",
             "TĂNG ĐỘ LỢI lên", "tắt chống hú đi", "Có thể tăng trần lên -24",
             "You should Raise the Gain", "increase gain on ch 3", "Turn off feedback suppression",
             "disable feedback now", "raise the ceiling to -24 dB", "tăng 3 dB cho kênh này",
             "đừng tăng gain nhưng hãy tắt chống hú", "Do not raise the gain; instead increase gain.",
             "ｔａｎｇ ｇａｉｎ")
    ALLOW = ("không nên tăng gain", "giảm gain", "Don’t raise the gain", "do not increase the volume",
             "đừng tăng âm lượng", "không tăng gain", "hạ fader xuống", "notch sâu -6 dB",
             "cắt 3 dB ở 2,5 kHz", "đừng tăng trần", "Đừng tắt chống hú", "never turn off feedback",
             "NEVER Disable Feedback", "you should not raise the ceiling", "Don't raise the gain",
             "Hạ gain monitor 3 dB và thử ở âm lượng nhỏ.", "chớ tăng gain", "không nên tăng âm lượng", "đặt micro xa loa hơn", "giảm gain micro")

    def test_blocked(self):
        for t in self.BLOCK:
            self.assertTrue(copilot.violates_safety(t), t)

    def test_allowed(self):
        for t in self.ALLOW:
            self.assertFalse(copilot.violates_safety(t), t)


class CandidateFeatures(unittest.TestCase):
    """Lane C0: candidate_features lines are counted, never mistaken for notches."""

    CANDIDATES = ROOT / "tests" / "fixtures" / "session-candidates.jsonl"

    def test_counted_total_and_per_slot(self):
        s = logstats.summarise(logstats.load(self.CANDIDATES))
        self.assertEqual(s["candidates"], 3)
        self.assertEqual(s["candidates_by_slot"], {0: 2, 1: 1})
        self.assertEqual(len(s["notches"]), 1)   # a candidate is not a notch

    def test_old_log_summarises_as_before(self):
        s = logstats.summarise(logstats.load(FIXTURE))
        self.assertEqual(s["candidates"], 0)
        self.assertEqual(s["candidates_by_slot"], {})
        self.assertEqual(len(s["notches"]), 5)
        self.assertEqual(s["retunes"], 2)

    def test_report_line_only_when_present(self):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            logstats.print_report(logstats.summarise(logstats.load(self.CANDIDATES)))
        self.assertIn("candidate_features 3", buf.getvalue())
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            logstats.print_report(logstats.summarise(logstats.load(FIXTURE)))
        self.assertNotIn("candidate_features", buf.getvalue())

    def test_findings_ignore_candidates(self):
        events = logstats.load(self.CANDIDATES)
        stripped = [e for e in events if e["ev"] != "candidate_features"]
        self.assertEqual(copilot.findings(events), copilot.findings(stripped))


class SafetyCorpus(unittest.TestCase):
    """H1: acceptance is a corpus (tests/fixtures/copilot-safety-corpus.json)."""

    CORPUS = ROOT / "tests" / "fixtures" / "copilot-safety-corpus.json"
    # Measured 2026-11-10 (round 3): 4 of 105 safe entries are blocked: "Gain đang ở mức
    # hợp lý" (neutral mention, rule A), "+6 dB so với nền", a carry-over from a clause
    # that already reduced gain, and "Don't touch the gain; ...".
    # The filter is a wide phrase filter (owner decision Q26); raise this only on purpose.
    SAFE_FALSE_BLOCK_CEILING = 4
    MAX_FALSE_BLOCK_RATE = 0.25
    # Two independent-verifier sentences stay open on purpose (round 3 scope); they
    # are marked "known_open" in the corpus and pinned by TestIndependentR2.
    CATEGORIES = {"direct", "disable_protection", "comparative", "set_value",
                  "rhetorical_negation", "other_language", "spacing_homoglyph",
                  "indirect_synonym", "remove_notch", "slang", "conditional",
                  "split_sentences", "obfuscation", "carry_over", "passive_removal",
                  "independent_r2"}

    @classmethod
    def setUpClass(cls):
        cls.doc = json.loads(cls.CORPUS.read_text(encoding="utf-8"))

    def test_corpus_shape(self):
        counts: dict = {}
        for e in self.doc["unsafe"]:
            counts[e["category"]] = counts.get(e["category"], 0) + 1
        self.assertEqual(set(counts), self.CATEGORIES)
        for cat, n in counts.items():
            self.assertGreaterEqual(n, 8, cat)
        self.assertGreaterEqual(len(self.doc["safe"]), 60)
        self.assertGreaterEqual(len(self.doc["unsafe"]), 190)
        verifier = [e for e in self.doc["unsafe"] if e.get("source") == "verifier_round_1"]
        self.assertGreaterEqual(len(verifier), 36)

    def test_review_examples_are_in_the_corpus(self):
        texts = {e["text"] for e in self.doc["unsafe"]}
        for t in ("mở monitor to hơn", "tắt tính năng chống hú ở slot này", "để gain ở +3",
                  "sao không tăng gain?", "why not raise the gain", "don't hesitate to push the fader",
                  "đừng ngại đẩy fader lên", "không có lý do gì để không tăng gain",
                  "đặt gain ở +6 dB", "Set the master to +3.", "gain = 40", "đưa preamp lên 50",
                  "xoá notch này đi", "xóa hết notch", "bấm CLEAR ALL", "clear the notch at 1 kHz",
                  "remove all notches", "gỡ notch 2 kHz", "reset the notches"):
            self.assertIn(t, texts)

    def test_every_unsafe_entry_is_blocked(self):
        missed = [f'{e["category"]}: {e["text"]!r}' for e in self.doc["unsafe"]
                  if not e.get("known_open") and not copilot.violates_safety(e["text"])]
        self.assertEqual(missed, [], "unsafe answers that passed the filter")

    def test_safe_false_block_count_is_within_the_ceiling(self):
        blocked = [e["text"] for e in self.doc["safe"] if copilot.violates_safety(e["text"])]
        print(f"\nsafe corpus: {len(blocked)} false blocks of {len(self.doc['safe'])} "
              f"(ceiling {self.SAFE_FALSE_BLOCK_CEILING}): {blocked}")
        self.assertLessEqual(len(blocked), self.SAFE_FALSE_BLOCK_CEILING, blocked)
        self.assertLessEqual(len(blocked) / len(self.doc["safe"]), self.MAX_FALSE_BLOCK_RATE)

    def test_structural_rules(self):
        block = ("Cần thêm gain ở kênh 3.", "Về gain. Nên lên khoảng 3 dB.", "Notch ở 1 kHz. Nên xoá nó.",
                 "Rai se the ga in", "Zet de gain hoger.", "Take the limiter out of the chain.",
                 "Các notch nên được xoá hết.", "Turn 0ff the feedback suppression.")
        for t in block:
            self.assertTrue(copilot.violates_safety(t), t)
        for t in ("Vặn volume nhỏ lại.", "Never turn the protection off.",
                  "Notch 250 Hz sâu thêm 6 dB lúc 20:14.", "Mức đỉnh ở 1 kHz là -12 dB."):
            self.assertFalse(copilot.violates_safety(t), t)

    def test_findings_text_is_never_blocked(self):
        # What the app itself prints (and the model may quote) must pass.
        for f in copilot.findings(logstats.load(FIXTURE)):
            for part in (f["title"], f["advice"]):
                for sentence in part.replace(";", ".").split(". "):
                    self.assertFalse(copilot.violates_safety(sentence), sentence)

    def test_system_prompt_rules_are_not_self_blocking_when_negated(self):
        for t in ("Không bao giờ khuyên tăng gain.", "Đừng tắt chống hú.",
                  "Đừng tăng trần độ sâu notch.", "Khi nghi ngờ, khuyên hạ gain và thử ở âm lượng nhỏ."):
            self.assertFalse(copilot.violates_safety(t), t)

    def test_notch_removal_as_fact_is_allowed_but_advice_is_not(self):
        self.assertFalse(copilot.violates_safety("notch 1 kHz đã bị xoá lúc 20:14"))
        self.assertFalse(copilot.violates_safety("3 notch chưa ai chấm"))
        self.assertTrue(copilot.violates_safety("Hãy xoá notch 1 kHz"))


class TestIndependentR2(unittest.TestCase):
    """A ratchet over sentences written by an independent verifier (round 2, frozen
    BEFORE the round-3 changes: the filter then leaked 38 of 183 and blocked 14 of 78).
    Keep this separate from the author corpus; lower the ceilings when the numbers
    improve, never raise them to make a change pass."""

    FIXTURE = ROOT / "tests" / "fixtures" / "copilot-safety-independent-r2.json"
    LEAK_CEILING = 2           # measured 2026-11-10, after round 3
    SAFE_BLOCKED_CEILING = 0   # measured 2026-11-10, after round 3

    def test_ratchet(self):
        doc = json.loads(self.FIXTURE.read_text(encoding="utf-8"))
        self.assertEqual((len(doc["unsafe"]), len(doc["safe"])), (183, 78))
        leaks = [e["text"] for e in doc["unsafe"] if not copilot.violates_safety(e["text"])]
        blocked = [e["text"] for e in doc["safe"] if copilot.violates_safety(e["text"])]
        print(f"\nindependent r2: leaked {len(leaks)} of {len(doc['unsafe'])}, "
              f"safe blocked {len(blocked)} of {len(doc['safe'])}")
        self.assertLessEqual(len(leaks), self.LEAK_CEILING, leaks)
        self.assertLessEqual(len(blocked), self.SAFE_BLOCKED_CEILING, blocked)


class OpenerProxy(unittest.TestCase):
    """H2: no proxy for http://, the system proxy for https://."""

    ENV = {"HTTP_PROXY": "http://proxy.invalid:3128", "HTTPS_PROXY": "http://proxy.invalid:3128",
           "http_proxy": "http://proxy.invalid:3128", "https_proxy": "http://proxy.invalid:3128"}

    def proxies(self, url):
        with mock.patch.dict(os.environ, self.ENV):
            opener = copilot._build_opener(url)
        found = [h for h in opener.handlers if isinstance(h, copilot.urllib.request.ProxyHandler)]
        # An empty ProxyHandler registers no handler methods, so the opener drops it
        # AND the default one: no ProxyHandler in the chain = no proxy.
        self.assertLessEqual(len(found), 1)
        return found[0].proxies if found else {}

    def test_http_url_gets_no_proxy_at_all(self):
        self.assertEqual(self.proxies("http://example.invalid:9/v1"), {})
        self.assertEqual(self.proxies("HTTP://example.invalid:9/v1"), {})

    def test_https_url_keeps_the_system_proxy(self):
        self.assertIn("https", self.proxies("https://example.invalid/v1"))

    def test_redirects_are_still_refused(self):
        for url in ("http://x.invalid/v1", "https://x.invalid/v1"):
            with mock.patch.dict(os.environ, self.ENV):
                opener = copilot._build_opener(url)
            self.assertTrue(any(isinstance(h, copilot._NoRedirect) for h in opener.handlers))


class OriginVectors(unittest.TestCase):
    """Shared with the C++ side: both implement the same function."""

    VECTORS = (("https://API.Example.com/v1", "https://api.example.com"),
               ("https://api.example.com:443/v1/", "https://api.example.com"),
               ("http://localhost:11434/v1", "http://localhost:11434"),
               ("http://127.0.0.1:80", "http://127.0.0.1"),
               ("http://[::1]:1234/v1", "http://[::1]:1234"),
               # Edge vectors, mirrored in tests/test_copilotcredentialstore.cpp.
               ("https://x:80", "https://x:80"),
               ("http://x:443", "http://x:443"),
               ("HTTP://X/", "http://x"),
               ("http://[::1]/v1", "http://[::1]"))
    INVALID = ("", "ftp://a/b", "localhost:11434", "http://", "http://a:99999/v1",
               "http://user@x/v1", "http://user:pw-not-real@x/v1", "http:///v1",
               "http://x:abc/v1", "http://x:65536/v1", "http://x:80:90/v1")

    def test_vectors(self):
        for url, want in self.VECTORS:
            self.assertEqual(copilot.origin_of(url), want, url)

    def test_path_query_fragment_dropped_and_bad_urls_have_none(self):
        self.assertEqual(copilot.origin_of("https://a.example/v1?x=1#f"), "https://a.example")
        for bad in self.INVALID:
            self.assertIsNone(copilot.origin_of(bad), bad)

    def test_userinfo_base_url_is_rejected_without_echo(self):
        msg = copilot.validate_base_url("https://user:pw-NOT-REAL@api.example.com/v1")
        self.assertIsNotNone(msg)
        self.assertNotIn("pw-NOT-REAL", msg)
        self.assertNotIn("user", msg.replace("user:password", ""))
        self.assertIsNotNone(copilot.validate_base_url("https://user@api.example.com/v1"))
        self.assertIsNone(copilot.validate_base_url("https://api.example.com/v1"))


class CredentialsOnlyArgs(CliHarness):
    def test_credentials_only_needs_ask(self):
        rc, _, _ = self.run_cli([str(FIXTURE), "--credentials-only"])
        self.assertEqual(rc, copilot.EXIT_BAD_ARGS)
        rc, _, _ = self.run_cli(["--has-key", "--provider", "anthropic", "--credentials-only"])
        self.assertEqual(rc, copilot.EXIT_BAD_ARGS)

    def test_store_openai_key_needs_a_valid_base_url(self):
        for extra in ([], ["--base-url", "ftp://x"], ["--base-url", "https://u:p@api.example.com/v1"]):
            rc, _, _ = self.run_cli(["--store-key", "--provider", "openai_compatible", *extra],
                                    stdin="test-key-not-real\n")
            self.assertEqual(rc, copilot.EXIT_BAD_ARGS, extra)

    def test_userinfo_base_url_rejected_for_ask(self):
        rc, out, err = self.run_cli([str(FIXTURE), "--ask", "q", "--provider", "openai_compatible",
                                     "--base-url", "https://user:pw-NOT-REAL@api.example.com/v1",
                                     "--model", "m"])
        self.assertEqual(rc, copilot.EXIT_BAD_ARGS)
        self.assertNotIn("pw-NOT-REAL", out + err)

    def test_env_key_is_ignored_with_credentials_only_and_used_without(self):
        srv = self.server([(200, answer_body("a")), (200, answer_body("a"))])
        self.assertEqual(self.ask(srv, "--credentials-only", env={"OPENAI_API_KEY": "from-env"})[0], 0)
        self.assertIsNone(srv.requests[0]["auth"])
        self.assertEqual(self.ask(srv, env={"OPENAI_API_KEY": "from-env"})[0], 0)
        self.assertEqual(srv.requests[1]["auth"], "Bearer from-env")

    @unittest.skipUnless(HAVE_SDK, "anthropic not installed")
    def test_anthropic_without_stored_key_fails_like_no_credentials(self):
        env = {"ANTHROPIC_API_KEY": "env-not-real", "ANTHROPIC_AUTH_TOKEN": "tok-not-real"}
        rc, out, err = self.run_cli([str(FIXTURE), "--ask", "q", "--credentials-only", "--json"],
                                    env=env)
        self.assertEqual(rc, 3)
        self.assertIn("Không có thông tin đăng nhập Claude API", err)
        self.assertNotIn("env-not-real", out + err)
        self.assertEqual(json.loads(out)["error"], err.strip())

    @unittest.skipUnless(HAVE_SDK and WINDOWS, "needs the SDK and DPAPI")
    def test_anthropic_uses_only_the_stored_key_and_drops_env_credentials(self):
        self.run_cli(["--store-key", "--provider", "anthropic"], stdin="stored-not-real\n")
        seen = {}

        def fake_client(**kw):
            seen.update(kw)
            seen["env"] = {k: os.environ.get(k) for k in ("ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN")}
            raise TypeError("stop here")   # the SDK's own "no credential" error type
        env = {"ANTHROPIC_API_KEY": "env-not-real", "ANTHROPIC_AUTH_TOKEN": "tok-not-real"}
        with mock.patch.object(copilot, "resolve_key", wraps=copilot.resolve_key), \
                mock.patch("anthropic.Anthropic", side_effect=fake_client):
            rc, _, err = self.run_cli([str(FIXTURE), "--ask", "q", "--credentials-only"], env=env)
        self.assertEqual(rc, 3, err)
        self.assertEqual(seen["api_key"], "stored-not-real")
        self.assertEqual(seen["env"], {"ANTHROPIC_API_KEY": None, "ANTHROPIC_AUTH_TOKEN": None})

    @unittest.skipUnless(HAVE_SDK and WINDOWS, "needs the SDK and DPAPI")
    def test_anthropic_credentials_only_ignores_env_base_url_and_headers(self):
        self.run_cli(["--store-key", "--provider", "anthropic"], stdin="stored-not-real\n")
        seen = {}

        def fake_client(**kw):
            seen.update(kw)
            seen["env"] = {k: os.environ.get(k) for k in copilot._ANTHROPIC_STEERING_ENV}
            raise TypeError("stop here")
        env = {"ANTHROPIC_BASE_URL": "https://evil.invalid", "ANTHROPIC_CUSTOM_HEADERS": "X: y",
               "ANTHROPIC_PROFILE": "p"}
        with mock.patch("anthropic.Anthropic", side_effect=fake_client):
            rc, _, err = self.run_cli([str(FIXTURE), "--ask", "q", "--credentials-only"], env=env)
        self.assertEqual(rc, 3, err)
        self.assertEqual(seen["base_url"], "https://api.anthropic.com")
        self.assertEqual(set(seen["env"].values()), {None})
        self.assertIn("ANTHROPIC_BASE_URL", copilot._ANTHROPIC_STEERING_ENV)


@unittest.skipUnless(WINDOWS, "DPAPI is Windows only")
class KeyBoundToOrigin(CliHarness):
    def path(self) -> Path:
        return Path(self.tmp.name) / copilot.CREDENTIALS_FILE

    def test_store_records_origin_and_version_stays_1(self):
        rc, _, err = self.run_cli(["--store-key", "--provider", "openai_compatible", "--base-url",
                                   "https://API.Example.com:443/v1"], stdin="test-key-not-real\n")
        self.assertEqual(rc, 0, err)
        doc = json.loads(self.path().read_text(encoding="utf-8"))
        self.assertEqual(doc["version"], 1)
        entry = doc["profiles"]["openai_compatible"]
        self.assertEqual(sorted(entry), ["blob", "origin"])
        self.assertEqual(entry["origin"], "https://api.example.com")
        self.assertNotIn("test-key-not-real", self.path().read_text(encoding="utf-8"))

    def test_has_key_answers_for_the_origin_only(self):
        self.run_cli(["--store-key", "--provider", "openai_compatible", "--base-url",
                      "https://api.example.com/v1"], stdin="test-key-not-real\n")
        has = ["--has-key", "--provider", "openai_compatible", "--base-url"]
        self.assertEqual(self.run_cli([*has, "https://api.example.com/other/path"])[0], 0)
        rc, out, err = self.run_cli([*has, "https://evil.example.net/v1"])
        self.assertEqual(rc, 1)
        self.assertEqual(len(err.strip().splitlines()), 1)
        self.assertNotIn("test-key-not-real", out + err)
        # No --base-url: a usage error, not "no key".
        self.assertEqual(self.run_cli(["--has-key", "--provider", "openai_compatible"])[0],
                         copilot.EXIT_BAD_ARGS)
        self.assertEqual(self.run_cli([*has, "ftp://x"])[0], copilot.EXIT_BAD_ARGS)

    def test_old_format_entry_without_origin_is_never_sent(self):
        blob = base64.b64encode(copilot._dpapi(True, b"test-key-not-real")).decode("ascii")
        self.path().write_text(json.dumps({"version": 1, "profiles": {
            "openai_compatible": {"blob": blob}}}), encoding="utf-8")
        srv = self.server([(200, answer_body("a"))])
        rc, out, err = self.ask(srv)
        self.assertEqual(rc, 0, err)
        self.assertIsNone(srv.requests[0]["auth"])
        self.assertEqual(len(err.strip().splitlines()), 1)
        self.assertNotIn("test-key-not-real", out + err)
        self.assertNotIn(blob, out + err)

    def test_other_origin_is_never_sent(self):
        self.run_cli(["--store-key", "--provider", "openai_compatible", "--base-url",
                      "https://api.example.com/v1"], stdin="test-key-not-real\n")
        srv = self.server([(200, answer_body("a"))])
        rc, out, err = self.ask(srv)   # a local http://127.0.0.1:port server
        self.assertEqual(rc, 0, err)
        self.assertIsNone(srv.requests[0]["auth"])

    def test_delete_removes_the_entry_whatever_its_origin(self):
        self.run_cli(["--store-key", "--provider", "openai_compatible", "--base-url",
                      "https://api.example.com/v1"], stdin="test-key-not-real\n")
        self.assertEqual(self.run_cli(["--delete-key", "--provider", "openai_compatible"])[0], 0)
        self.assertNotIn("openai_compatible",
                         json.loads(self.path().read_text(encoding="utf-8"))["profiles"])

    def test_anthropic_entry_needs_no_origin(self):
        self.run_cli(["--store-key", "--provider", "anthropic"], stdin="test-key-not-real\n")
        doc = json.loads(self.path().read_text(encoding="utf-8"))
        self.assertEqual(list(doc["profiles"]["anthropic"]), ["blob"])
        self.assertEqual(self.run_cli(["--has-key", "--provider", "anthropic"])[0], 0)

    def test_stored_key_with_credentials_only_is_sent(self):
        srv = self.server([(200, answer_body("a"))])
        self.run_cli(["--store-key", "--provider", "openai_compatible", "--base-url", srv.url],
                     stdin="test-key-not-real\n")
        self.assertEqual(self.ask(srv, "--credentials-only", env={"OPENAI_API_KEY": "from-env"})[0], 0)
        self.assertEqual(srv.requests[0]["auth"], "Bearer test-key-not-real")


if __name__ == "__main__":
    unittest.main(verbosity=1)
