#!/usr/bin/env python3
"""Summarise a Hands-free session log (lane D, data-loop design §3.5; reads log schema 1-4).

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


AUTHORS = ("detector", "human", "network", "llm")          # log schema 4 `author` values
AI_EVENTS = ("ai_mode", "ai_call", "ai_proposal", "ai_decision")   # log schema 4 event kinds


def effective_author(e: dict, origin) -> str:
    """Who did this notch line. Log schema 4 writes `author` on every notch_set /
    notch_retune / notch_clear line. A line WITHOUT one (schema 1-3) reads as
    `detector` when the notch's origin is `detector` and as `human` otherwise
    (preset / manual / soundcheck): that is what those logs meant, and it keeps
    every old log reading exactly as it did before schema 4."""
    a = e.get("author")
    if isinstance(a, str) and a:
        return a
    return "detector" if origin == "detector" else "human"


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
                 "retunes": 0,
                 # log schema 4: who placed it (effective author, see above) and,
                 # when the line carries one, its notch id.
                 "author": effective_author(e, e.get("origin", "?")),
                 # deepest depth reached by the detector itself (the placement
                 # and any detector-authored retune): what the copilot's ceiling
                 # rule reads. Equals deepest_db on every schema 1-3 log.
                 "detector_deepest_db": e.get("depth_db")}
            if "nid" in e:
                n["nid"] = e["nid"]
            # A Set onto a key whose previous record never saw its Clear (a
            # lost Clear, a log cut short) closes that record here: left open
            # it reads "still active" for the whole session (review
            # 2026-10-20, M9).
            old = open_by_key.get(key(e))
            if old is not None:
                old["clear_t"] = n["set_t"]
                old["reason"] = "superseded"
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
                    if (effective_author(e, n["origin"]) == "detector"
                            and (n["detector_deepest_db"] is None
                                 or float(d) < float(n["detector_deepest_db"]))):
                        n["detector_deepest_db"] = d
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

    # Lane C0: near-miss candidates (log_schema 2). Counted, never joined to a
    # notch -- by definition no notch exists for them. An old log has none.
    candidates_by_slot: dict[int, int] = {}
    for e in events:
        if e.get("ev") == "candidate_features":
            slot = int(e.get("slot", 0))
            candidates_by_slot[slot] = candidates_by_slot.get(slot, 0) + 1

    # Lane C1b (log_schema 3): lines carrying a shadow-classifier opinion. The
    # field is absent (not 0) when there was none, and schema 2 logs have none.
    nn_by_ev: dict[str, int] = {}
    for e in events:
        if "nn_score" in e:
            nn_by_ev[e.get("ev", "?")] = nn_by_ev.get(e.get("ev", "?"), 0) + 1

    # Log schema 4: placements per author (effective author of each notch_set
    # line) and the four AI event kinds. Both are zero / empty on schema 1-3.
    author_placements: dict[str, int] = {}
    for n in notches:
        author_placements[n["author"]] = author_placements.get(n["author"], 0) + 1
    ai_events = {k: sum(1 for e in events if e.get("ev") == k) for k in AI_EVENTS}

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
        "superseded": sum(1 for n in notches if n["reason"] == "superseded"),
        "dropped": end.get("dropped_events"),
        "candidates": sum(candidates_by_slot.values()),
        "candidates_by_slot": candidates_by_slot,
        "dropped_candidates": end.get("dropped_candidates"),
        "nn_scored": sum(nn_by_ev.values()),
        "nn_scored_by_ev": nn_by_ev,
        "nn_model": header.get("nn_model"),
        "nn_feature_set": header.get("nn_feature_set"),
        "nn_calls": end.get("nn_calls"),
        "nn_us_p99": end.get("nn_us_p99"),
        "nn_us_max": end.get("nn_us_max"),
        "log_schema": header.get("log_schema"),
        "ai_mode": header.get("ai_mode"),
        "author_placements": dict(sorted(author_placements.items())),
        "ai_events": ai_events,
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
    if s["superseded"]:
        print(f"{s['superseded']} notch record(s) closed by a later Set on the same slot "
              "(their Clear is missing from the log)")
    if s["dropped"]:
        print(f"WARNING: logger dropped {s['dropped']} event(s)")
    # Lane C0: printed only when the log has candidate_features, so an old log
    # reads exactly as it always did.
    if s["candidates"]:
        per_slot = "  ".join(f"slot {k}: {v}" for k, v in sorted(s["candidates_by_slot"].items()))
        print(f"candidate_features {s['candidates']}  ({per_slot})")
    if s["dropped_candidates"]:
        print(f"WARNING: {s['dropped_candidates']} candidate record(s) dropped (own queue)")
    # Lane C1b: printed only for a schema-3 log that ran a classifier (or has
    # any nn_score), so schema 1 / 2 logs read exactly as they always did.
    if s["nn_model"] is not None or s["nn_scored"]:
        by_ev = "  ".join(f"{k}: {v}" for k, v in sorted(s["nn_scored_by_ev"].items()))
        print(f"nn model {s['nn_model'] or '?'}  feature_set {s['nn_feature_set'] if s['nn_feature_set'] is not None else '?'}"
              f"  nn_score lines {s['nn_scored']}" + (f"  ({by_ev})" if by_ev else ""))
        if s["nn_calls"] is not None:
            print(f"nn calls {s['nn_calls']}  p99 {s['nn_us_p99']} us  max {s['nn_us_max']} us")


    # Log schema 4: printed only for a log that has it (or any AI event), so a
    # schema 1-3 log reads exactly as it always did.
    schema = s["log_schema"]
    ai_total = sum(s["ai_events"].values())
    if (isinstance(schema, int) and not isinstance(schema, bool) and schema >= 4) or ai_total:
        by_author = "  ".join(f"{a}: {s['author_placements'].get(a, 0)}"
                              for a in AUTHORS)
        other = {a: c for a, c in s["author_placements"].items() if a not in AUTHORS}
        if other:
            by_author += "  " + "  ".join(f"{a}: {c}" for a, c in other.items())
        print(f"ai_mode {s['ai_mode'] or '?'}  placements by author  {by_author}")
        print("ai events  " + "  ".join(f"{k}: {v}" for k, v in s["ai_events"].items()))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path)
    ap.add_argument("--expect-notches", type=int)
    ap.add_argument("--expect-verdicts", type=int)
    ap.add_argument("--expect-false", type=int)
    ap.add_argument("--expect-recurrence-max", type=int)
    ap.add_argument("--expect-retunes", type=int)
    ap.add_argument("--expect-soundcheck-replaced", type=int)
    ap.add_argument("--expect-superseded", type=int)
    ap.add_argument("--expect-candidates", type=int)
    ap.add_argument("--expect-nn-scored", type=int)
    ap.add_argument("--expect-nn-model", type=str)
    ap.add_argument("--expect-ai-events", type=int, help="total ai_mode/ai_call/ai_proposal/ai_decision lines")
    ap.add_argument("--expect-authors", type=str, metavar="A=N,...",
                    help="notch placements per author, e.g. detector=2,human=1 (listed authors only)")
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
    if args.expect_superseded is not None and s["superseded"] != args.expect_superseded:
        failures.append(f"superseded {s['superseded']} != {args.expect_superseded}")
    if args.expect_candidates is not None and s["candidates"] != args.expect_candidates:
        failures.append(f"candidates {s['candidates']} != {args.expect_candidates}")
    if args.expect_nn_scored is not None and s["nn_scored"] != args.expect_nn_scored:
        failures.append(f"nn-scored {s['nn_scored']} != {args.expect_nn_scored}")
    if args.expect_nn_model is not None and s["nn_model"] != args.expect_nn_model:
        failures.append(f"nn-model {s['nn_model']} != {args.expect_nn_model}")
    if args.expect_ai_events is not None and sum(s["ai_events"].values()) != args.expect_ai_events:
        failures.append(f"ai-events {sum(s['ai_events'].values())} != {args.expect_ai_events}")
    if args.expect_authors is not None:
        for pair in args.expect_authors.split(","):
            name, _, want = pair.partition("=")
            if not want.isdigit() or s["author_placements"].get(name, 0) != int(want):
                failures.append(f"author {name} {s['author_placements'].get(name, 0)} != {want}")
    if args.expect_recurrence_max is not None:
        recurrence_max = max((g["count"] for g in s["groups"]), default=0)
        if recurrence_max != args.expect_recurrence_max:
            failures.append(f"recurrence-max {recurrence_max} != {args.expect_recurrence_max}")
    for f in failures:
        print("EXPECT FAILED: " + f, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
