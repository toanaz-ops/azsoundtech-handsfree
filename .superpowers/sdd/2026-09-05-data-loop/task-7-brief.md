### Task 7: `tools/logstats.py`, fixture, ctest registration

**Files:**
- Create: `tools/logstats.py`, `tests/fixtures/session-sample.jsonl`
- Modify: `tests/CMakeLists.txt` (after `gtest_discover_tests`)

**Interfaces:**
- Consumes: the event contract table in Task 6.
- Produces: `python tools/logstats.py <file.jsonl>` prints the four sections of spec §3.5; `--expect-notches N --expect-verdicts N --expect-false N` make it exit 1 on mismatch (the ctest hook).

- [ ] **Step 1: Write the fixture** — `tests/fixtures/session-sample.jsonl` (spectra shortened to `bins: 4` so the file stays readable; the tool must never assume 1025):

```json
{"ev":"session_start","t":0,"app_version":"1.1.2","os":"Windows 11","device":"Fake ASIO","sample_rate":48000,"buffer_size":256,"slots":[{"index":0,"enabled":true,"width":2,"in":[0,1],"out":[0,1],"linked":false}]}
{"ev":"mode","t":12.5,"mode":"auto"}
{"ev":"tuning","t":13.0,"slot":-1,"rise_ms":250,"persist":3,"q":30,"depth_db":-18,"thr":10}
{"ev":"notch_set","t":5001.0,"slot":0,"lane":1,"index":0,"hz":1007.8,"q":30,"depth_db":-18,"origin":"detector","confirmed_lane":1,"score":0.91,"peakiness":22.1,"p_norm":0.13,"rise":1,"novelty":1,"penalty":1,"asymmetry":1,"persist_needed":3,"thr":10,"ctx":{"bins":4,"bin_hz":12000,"ref_age_ms":117.3,"now":[0.01,0.5,0.02,0.01],"ref":[0.01,0.1,0.02,0.01],"other_lane_now":[0.01,0.02,0.02,0.01]}}
{"ev":"notch_set","t":9020.0,"slot":0,"lane":0,"index":0,"hz":2437.5,"q":30,"depth_db":-18,"origin":"detector","confirmed_lane":0,"score":0.75,"peakiness":15.0,"p_norm":0.06,"rise":1,"novelty":1,"penalty":1,"asymmetry":1,"persist_needed":3,"thr":10,"ctx":{"bins":4,"bin_hz":12000,"ref_age_ms":128.0,"now":[0.01,0.02,0.4,0.01],"ref":[0.01,0.02,0.1,0.01]}}
{"ev":"verdict","t":11000.0,"slot":0,"lane":0,"index":0,"hz":2437.5,"verdict":"false","age_ms":1980.0}
{"ev":"notch_clear","t":11000.5,"slot":0,"lane":0,"index":0,"hz":2437.5,"origin":"detector","reason":"verdict_false","age_ms":1980.5}
{"ev":"notch_set","t":20000.0,"slot":0,"lane":1,"index":1,"hz":1007.8,"q":30,"depth_db":-18,"origin":"detector","confirmed_lane":1,"score":0.88,"peakiness":20.0,"p_norm":0.11,"rise":1,"novelty":1,"penalty":1,"asymmetry":1,"persist_needed":3,"thr":10,"ctx":{"bins":4,"bin_hz":12000,"ref_age_ms":120.0,"now":[0.01,0.45,0.02,0.01],"ref":[0.01,0.1,0.02,0.01]}}
{"ev":"verdict","t":21000.0,"slot":0,"lane":1,"index":1,"hz":1007.8,"verdict":"good","age_ms":1000.0}
{"ev":"notch_set","t":30000.0,"slot":0,"lane":0,"index":2,"hz":482.0,"q":30,"depth_db":-12,"origin":"preset"}
{"ev":"verdict","t":31000.0,"slot":0,"lane":0,"index":2,"hz":482.0,"verdict":"good","age_ms":1000.0}
{"ev":"notch_clear","t":41000.0,"slot":0,"lane":1,"index":0,"hz":1007.8,"origin":"detector","reason":"auto_release","age_ms":36000.0}
{"ev":"session_end","t":60000.0,"dropped_events":0}
```

Expected summary: 4 notches placed, 3 verdicts (1 false, 2 good), 1 notch never judged (lane 1 index 1 at 20 s... no: that one is judged good; the unjudged one is lane 1 index 0 at 5 s), recurrence group at 1007.8 Hz = 2 placements.

- [ ] **Step 2: Write the tool** — `tools/logstats.py`:

```python
#!/usr/bin/env python3
"""Summarise a Hands-free session log (lane D, data-loop design §3.5).

    python tools/logstats.py <session-*.jsonl>
    python tools/logstats.py <file> --expect-notches 4 --expect-verdicts 3 --expect-false 1

Stdlib only. Never assumes 1025 bins; never reads audio (there is none).
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


def load(path: Path) -> list[dict]:
    events = []
    with path.open("r", encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as exc:
                print(f"line {n}: not JSON ({exc}); skipped", file=sys.stderr)
    return events


def fmt_ms(ms: float) -> str:
    s = ms / 1000.0
    return f"{s:.1f}s" if s < 60 else f"{int(s // 60)}m{int(s % 60):02d}s"


def summarise(events: list[dict]) -> dict:
    header = next((e for e in events if e.get("ev") == "session_start"), {})
    end = next((e for e in events if e.get("ev") == "session_end"), {})
    duration = float(end.get("t", events[-1].get("t", 0.0) if events else 0.0))

    modes = [(float(e["t"]), e.get("mode", "?")) for e in events if e.get("ev") == "mode"]

    key = lambda e: (int(e.get("slot", 0)), int(e.get("lane", 0)), int(e.get("index", 0)))
    notches: list[dict] = []          # one per notch_set, in order
    open_by_key: dict[tuple, dict] = {}
    for e in events:
        ev = e.get("ev")
        if ev == "notch_set":
            n = {"slot": e.get("slot"), "lane": e.get("lane"), "index": e.get("index"),
                 "hz": float(e.get("hz", 0.0)), "origin": e.get("origin", "?"),
                 "set_t": float(e.get("t", 0.0)), "clear_t": None, "reason": None,
                 "verdict": None, "score": e.get("score")}
            notches.append(n)
            open_by_key[key(e)] = n
        elif ev == "verdict":
            n = open_by_key.get(key(e))
            if n is not None:
                n["verdict"] = e.get("verdict")
        elif ev == "notch_clear":
            n = open_by_key.pop(key(e), None)
            if n is not None:
                n["clear_t"] = float(e.get("t", 0.0))
                n["reason"] = e.get("reason")

    # Recurrence: group by Hz within +-1 bin (bin width from the first ctx, else 23.4375).
    bin_hz = 23.4375
    for e in events:
        if e.get("ev") == "notch_set" and isinstance(e.get("ctx"), dict) and "bin_hz" in e["ctx"]:
            bin_hz = float(e["ctx"]["bin_hz"])
            break
    groups: list[dict] = []
    for n in notches:
        for g in groups:
            if abs(g["hz"] - n["hz"]) <= bin_hz:
                g["count"] += 1
                break
        else:
            groups.append({"hz": n["hz"], "count": 1})

    judged = [n for n in notches if n["verdict"] in ("good", "false")]
    false_count = sum(1 for n in judged if n["verdict"] == "false")
    return {
        "header": header, "duration_ms": duration, "modes": modes, "notches": notches,
        "groups": sorted(groups, key=lambda g: -g["count"]),
        "verdicts": len(judged), "false": false_count,
        "unjudged": len(notches) - len(judged),
        "dropped": end.get("dropped_events"),
    }


def print_report(s: dict) -> None:
    h = s["header"]
    print(f"session   {fmt_ms(s['duration_ms'])}  app {h.get('app_version', '?')}  os {h.get('os', '?')}")
    print(f"device    {h.get('device', '?')!r}  {h.get('sample_rate', '?')} Hz  buffer {h.get('buffer_size', '?')}")
    if s["modes"]:
        print("modes     " + "  ".join(f"{fmt_ms(t)}:{m}" for t, m in s["modes"]))
    print()
    print(f"{'#':>3} {'slot':>4} {'lane':>4} {'hz':>8} {'origin':<10} {'held':>8} {'verdict':<8} {'cleared by':<20}")
    for i, n in enumerate(s["notches"], 1):
        held = (n["clear_t"] if n["clear_t"] is not None else s["duration_ms"]) - n["set_t"]
        lane = "R" if n["lane"] == 1 else "L"
        print(f"{i:>3} {n['slot']:>4} {lane:>4} {n['hz']:>8.1f} {n['origin']:<10} {fmt_ms(held):>8} "
              f"{(n['verdict'] or '-'):<8} {(n['reason'] or 'still active'):<20}")
    print()
    print("recurrence (Hz within one bin, count of placements):")
    for g in s["groups"]:
        if g["count"] > 1:
            print(f"  {g['hz']:>8.1f} Hz  x{g['count']}")
    if not any(g["count"] > 1 for g in s["groups"]):
        print("  none")
    print()
    total = len(s["notches"])
    v = s["verdicts"]
    print(f"notches {total}  judged {v}  false {s['false']}"
          f"  false-rate {(s['false'] / v * 100 if v else 0):.0f}%"
          f"  unjudged {s['unjudged']} ({(s['unjudged'] / total * 100 if total else 0):.0f}%)")
    if s["dropped"]:
        print(f"WARNING: logger dropped {s['dropped']} event(s)")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path)
    ap.add_argument("--expect-notches", type=int)
    ap.add_argument("--expect-verdicts", type=int)
    ap.add_argument("--expect-false", type=int)
    args = ap.parse_args(argv)

    events = load(args.file)
    if not events:
        print("empty or unreadable log", file=sys.stderr)
        return 2
    s = summarise(events)
    print_report(s)

    failures = []
    if args.expect_notches is not None and len(s["notches"]) != args.expect_notches:
        failures.append(f"notches {len(s['notches'])} != {args.expect_notches}")
    if args.expect_verdicts is not None and s["verdicts"] != args.expect_verdicts:
        failures.append(f"verdicts {s['verdicts']} != {args.expect_verdicts}")
    if args.expect_false is not None and s["false"] != args.expect_false:
        failures.append(f"false {s['false']} != {args.expect_false}")
    for f in failures:
        print("EXPECT FAILED: " + f, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
```

- [ ] **Step 3: Run it by hand** — `python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-notches 4 --expect-verdicts 3 --expect-false 1`. Expected: the report, exit 0, recurrence shows `1007.8 Hz x2`, "unjudged 1 (25%)". Then `python tools/logstats.py tests/fixtures/session-sample.jsonl --expect-false 2` → exit 1 with `EXPECT FAILED`.

- [ ] **Step 4: Register in ctest** — append to `tests/CMakeLists.txt`:

```cmake
# Lane D: the log summary tool runs against a checked-in fixture. Python is
# optional on a build box; without it the test SKIPS rather than fails.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME logstats_fixture
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/logstats.py
                ${CMAKE_SOURCE_DIR}/tests/fixtures/session-sample.jsonl
                --expect-notches 4 --expect-verdicts 3 --expect-false 1)
else()
    add_test(NAME logstats_fixture COMMAND ${CMAKE_COMMAND} -E echo "SKIP: python3 not found")
    set_tests_properties(logstats_fixture PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
endif()
```

- [ ] **Step 5: Reconfigure and run** — `cmake -B build -G "Visual Studio 18 2026" -A x64 && cd build && ctest -C Release --output-on-failure -R logstats`. Expected: `1/1 Test #...: logstats_fixture ... Passed`.

- [ ] **Step 6: Full suite** — `100% tests passed` (425).

- [ ] **Step 7: Commit**

```bash
git add tools/logstats.py tests/fixtures/session-sample.jsonl tests/CMakeLists.txt
git commit -m "feat(tools): logstats.py summarises a session log; fixture runs under ctest"
```

---

