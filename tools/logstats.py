#!/usr/bin/env python3
"""Summarise a Hands-free session log (lane D, data-loop design §3.5).

    python tools/logstats.py <session-*.jsonl>
    python tools/logstats.py <file> --expect-notches 4 --expect-verdicts 3 --expect-false 1 --expect-recurrence-max 2 --expect-retunes 2

Stdlib only. Never assumes 1025 bins; never reads audio (there is none).
"""
from __future__ import annotations

import argparse
import json
import sys
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
                 "verdict": None, "score": e.get("score"),
                 # lane G: the depth a notch is RUNNING at, and how many times
                 # it moved. Both start at the placement values.
                 "depth_db": e.get("depth_db"), "deepest_db": e.get("depth_db"),
                 "retunes": 0}
            notches.append(n)
            open_by_key[key(e)] = n
        elif ev == "notch_retune":
            # lane G: a retune UPDATES the open record. It must never close it
            # -- a deepening 300 ms after placement would otherwise read as a
            # 300 ms notch, and every deepened howl in the log would look like
            # a false positive. An unknown ev name falls through every branch
            # here, so a NEW event added later cannot corrupt an old reader
            # either; that is why this is an if/elif chain and not a lookup
            # that raises.
            n = open_by_key.get(key(e))
            if n is not None:
                # depth_db is guarded the same way deepest_db is: a truncated
                # or hand-edited line without it must not erase the running
                # depth the notch_set established. The retune still counts --
                # it happened, we just cannot say what it moved to.
                d = e.get("depth_db")
                if d is not None:
                    n["depth_db"] = d
                    if n["deepest_db"] is None or float(d) < float(n["deepest_db"]):
                        n["deepest_db"] = d
                n["retunes"] += 1
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
        "retunes": sum(n["retunes"] for n in notches),
        # Lane M: how many notches this session's soundchecks replaced with
        # their own next proposal. Counted here rather than left to the reason
        # column because "the soundcheck keeps replacing its own work" is a
        # pattern worth seeing at a glance, and because a fixture can then pin
        # that the reason name survives the whole C++ -> JSON -> reader path.
        "soundcheck_replaced": sum(1 for n in notches if n["reason"] == "soundcheck_replace"),
        "dropped": end.get("dropped_events"),
    }


def print_report(s: dict) -> None:
    h = s["header"]
    print(f"session   {fmt_ms(s['duration_ms'])}  app {h.get('app_version', '?')}  os {h.get('os', '?')}")
    print(f"device    {h.get('device', '?')!r}  {h.get('sample_rate', '?')} Hz  buffer {h.get('buffer_size', '?')}")
    if s["modes"]:
        print("modes     " + "  ".join(f"{fmt_ms(t)}:{m}" for t, m in s["modes"]))
    print()
    print(f"{'#':>3} {'slot':>4} {'lane':>4} {'hz':>8} {'origin':<10} {'depth':>7} {'deep':>6} "
          f"{'rt':>3} {'held':>8} {'verdict':<8} {'cleared by':<20}")
    for i, n in enumerate(s["notches"], 1):
        held = (n["clear_t"] if n["clear_t"] is not None else s["duration_ms"]) - n["set_t"]
        lane = "R" if n["lane"] == 1 else "L"
        depth = "?" if n["depth_db"] is None else f"{float(n['depth_db']):.0f}dB"
        deep = "?" if n["deepest_db"] is None else f"{float(n['deepest_db']):.0f}dB"
        print(f"{i:>3} {n['slot']:>4} {lane:>4} {n['hz']:>8.1f} {n['origin']:<10} "
              f"{depth:>7} {deep:>6} {n['retunes']:>3} {fmt_ms(held):>8} "
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
    print(f"notches {total}  retunes {s['retunes']}  judged {v}  false {s['false']}"
          f"  false-rate {(s['false'] / v * 100 if v else 0):.0f}%"
          f"  unjudged {s['unjudged']} ({(s['unjudged'] / total * 100 if total else 0):.0f}%)")
    if s["soundcheck_replaced"]:
        print(f"soundcheck replaced {s['soundcheck_replaced']} notch(es) from an earlier run")
    if s["dropped"]:
        print(f"WARNING: logger dropped {s['dropped']} event(s)")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path)
    ap.add_argument("--expect-notches", type=int)
    ap.add_argument("--expect-verdicts", type=int)
    ap.add_argument("--expect-false", type=int)
    ap.add_argument("--expect-recurrence-max", type=int)
    ap.add_argument("--expect-retunes", type=int)
    ap.add_argument("--expect-soundcheck-replaced", type=int)
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
    if args.expect_retunes is not None and s["retunes"] != args.expect_retunes:
        failures.append(f"retunes {s['retunes']} != {args.expect_retunes}")
    if (args.expect_soundcheck_replaced is not None
            and s["soundcheck_replaced"] != args.expect_soundcheck_replaced):
        failures.append(
            f"soundcheck-replaced {s['soundcheck_replaced']} != {args.expect_soundcheck_replaced}")
    if args.expect_recurrence_max is not None:
        recurrence_max = max((g["count"] for g in s["groups"]), default=0)
        if recurrence_max != args.expect_recurrence_max:
            failures.append(f"recurrence-max {recurrence_max} != {args.expect_recurrence_max}")
    for f in failures:
        print("EXPECT FAILED: " + f, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
