#!/usr/bin/env python3
"""Soundman copilot over a Hands-free session log (lane L, step 1 -- offline).

    python tools/copilot/copilot.py <session-*.jsonl>            # findings, no network
    python tools/copilot/copilot.py <file> --json                # same, machine-readable
    python tools/copilot/copilot.py <file> --ask "Vì sao 1 kHz hú hoài?"

The findings are computed here, deterministically, from the log; they need no
network and no LLM. `--ask` hands the SAME data to Claude through read-only
tools so a soundman can ask questions in plain words. The model can only read
what these tools return -- it cannot touch the app, the mixer, or the log.

Never runs inside the app and never on the audio path: the app is a live-sound
insert, and a network call has no place in it (spec §2,
docs/superpowers/specs/2026-10-20-llm-copilot-design.md).
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import logstats  # noqa: E402  (reuse the lane D reader: one parser, one truth)

# --- thresholds (spec §4). Each is a named constant so a test can pin it. ----
RECUR_WARN = 3            # placements at one frequency before it is a warning
STACK_OCTAVES = 1.0 / 6   # two live notches closer than this on one lane = "stacked"
BURST_COUNT = 4           # detector placements ...
BURST_WINDOW_MS = 60_000  # ... inside this window = running at the edge
CEILING_SLACK_DB = 0.5    # a notch this close to the ceiling has hit it
FALSE_RATE_WARN = 0.3     # share of judged notches marked false
FALSE_MIN_JUDGED = 3      # below this many verdicts a rate means nothing
UNJUDGED_SHARE = 0.5      # share of notches with no verdict at all
UNJUDGED_MIN = 3

SEVERITY_ORDER = {"alert": 0, "warn": 1, "info": 2}


def _lane(n: dict) -> str:
    return "R" if n.get("lane") == 1 else "L"


def _where(n: dict) -> str:
    return f"slot {n.get('slot')} làn {_lane(n)}"


def _ceilings(events: list[dict]) -> dict[int, float]:
    """Deepest ceiling ever set, per slot; key -1 = every slot.

    The tuning event carries the slider ceiling as `depth_db` (negative dB).
    The deepest one in the session is used: a notch that reached THAT reached
    the ceiling whatever the slider said later."""
    out: dict[int, float] = {}
    for e in events:
        if e.get("ev") == "tuning" and isinstance(e.get("depth_db"), (int, float)):
            s = int(e.get("slot", -1))
            d = float(e["depth_db"])
            out[s] = min(out.get(s, d), d)
    return out


def findings(events: list[dict], summary: dict | None = None) -> list[dict]:
    """Every finding this log supports, most severe first.

    Each is {code, severity, title, advice, evidence}. Advice is Vietnamese
    and is ADVICE: nothing here changes anything. No rule ever advises raising
    gain or removing a protection (spec §4.2)."""
    s = summary if summary is not None else logstats.summarise(events)
    notches = s["notches"]
    out: list[dict] = []

    # 1. Recurrence: the same frequency placed again and again.
    for g in s["groups"]:
        if g["count"] >= 2:
            sev = "warn" if g["count"] >= RECUR_WARN else "info"
            out.append({
                "code": "recurrence", "severity": sev,
                "title": f"{g['hz']:.0f} Hz hú lại {g['count']} lần",
                "advice": ("Cùng một tần số bị đặt notch nhiều lần nghĩa là nguyên nhân vẫn còn: "
                           "kiểm tra hướng mic so với loa monitor, hoặc giảm dải này trên EQ "
                           "monitor. Nếu phòng luôn hú ở đây, lưu nó vào preset."),
                "evidence": {"hz": round(g["hz"], 1), "count": g["count"]},
            })

    # 2. Ceiling: a notch that reached the slider's depth.
    ceilings = _ceilings(events)
    for n in notches:
        # A notch the soundman called false is not a loop-gain problem, however
        # deep it went.
        if n["origin"] != "detector" or n["deepest_db"] is None or n["verdict"] == "false":
            continue
        ceil = ceilings.get(int(n["slot"] or 0), ceilings.get(-1))
        if ceil is not None and float(n["deepest_db"]) <= ceil + CEILING_SLACK_DB:
            out.append({
                "code": "ceiling", "severity": "alert",
                "title": f"{n['hz']:.0f} Hz chạm trần {ceil:.0f} dB ({_where(n)})",
                "advice": ("Notch đã sâu tới mức tối đa cho phép: độ lợi vòng ở tần số này cao. "
                           "Hạ gain monitor hoặc kênh đó vài dB; đừng tăng trần."),
                "evidence": {"hz": round(n["hz"], 1), "slot": n["slot"], "lane": _lane(n),
                             "deepest_db": n["deepest_db"], "ceiling_db": ceil},
            })

    # 3. Stacked: two notches alive at once, very close, on the same lane.
    open_now: list[dict] = []
    for n in sorted(notches, key=lambda x: x["set_t"]):
        open_now = [o for o in open_now if o["clear_t"] is None or o["clear_t"] > n["set_t"]]
        for o in open_now:
            if o["slot"] != n["slot"] or o["lane"] != n["lane"] or o["hz"] <= 0 or n["hz"] <= 0:
                continue
            octs = abs(math.log2(n["hz"] / o["hz"]))
            if 0 < octs <= STACK_OCTAVES:
                lo, hi = sorted((o["hz"], n["hz"]))
                out.append({
                    "code": "stacked", "severity": "warn",
                    "title": f"Notch chồng sát nhau {lo:.0f}–{hi:.0f} Hz ({_where(n)})",
                    "advice": ("Hai notch hẹp sát nhau thường là một vùng cộng hưởng rộng: "
                               "một nhát EQ rộng trên mixer xử lý gọn hơn nhiều notch hẹp."),
                    "evidence": {"hz": [round(lo, 1), round(hi, 1)], "slot": n["slot"],
                                 "lane": _lane(n), "octaves": round(octs, 3)},
                })
        open_now.append(n)

    # 4. Burst: many detector placements in a short window.
    det_t = sorted(n["set_t"] for n in notches if n["origin"] == "detector")
    best = (0, 0.0)
    j = 0
    for i, t in enumerate(det_t):
        while t - det_t[j] > BURST_WINDOW_MS:
            j += 1
        if i - j + 1 > best[0]:
            best = (i - j + 1, det_t[j])
    if best[0] >= BURST_COUNT:
        out.append({
            "code": "burst", "severity": "alert",
            "title": f"{best[0]} notch trong {BURST_WINDOW_MS // 1000} s từ {logstats.fmt_ms(best[1])}",
            "advice": ("Hệ thống đang chạy sát biên hú: hạ tổng gain hoặc monitor 2–3 dB rồi "
                       "chạy ĐO PHÒNG lại."),
            "evidence": {"count": best[0], "window_ms": BURST_WINDOW_MS, "from_ms": best[1]},
        })

    # 5. False positives the soundman told us about.
    v = s["verdicts"]
    if v >= FALSE_MIN_JUDGED and s["false"] / v >= FALSE_RATE_WARN:
        out.append({
            "code": "false_rate", "severity": "warn",
            "title": f"{s['false']}/{v} notch bị đánh dấu oan",
            "advice": ("Chống hú đang cắt nhầm nhạc khá nhiều: thử preset Safe hoặc Music, "
                       "và gửi log này cho nhóm phát triển."),
            "evidence": {"false": s["false"], "judged": v},
        })

    # 6. Missing labels: the data lane C waits for.
    total = len(notches)
    if total >= UNJUDGED_MIN and s["unjudged"] / total >= UNJUDGED_SHARE:
        out.append({
            "code": "unjudged", "severity": "info",
            "title": f"{s['unjudged']}/{total} notch chưa được đánh giá",
            "advice": ("Bấm oan/đúng trên danh sách notch khi có thể: mỗi nhãn giúp bản sau "
                       "phân biệt hú với nốt nhạc."),
            "evidence": {"unjudged": s["unjudged"], "total": total},
        })

    # 7. The log itself is incomplete.
    if s["dropped"]:
        out.append({
            "code": "dropped", "severity": "warn",
            "title": f"Log mất {s['dropped']} sự kiện",
            "advice": "Các kết luận trên có thể thiếu: log không ghi kịp mọi sự kiện.",
            "evidence": {"dropped": s["dropped"]},
        })

    out.sort(key=lambda f: SEVERITY_ORDER[f["severity"]])
    return out


def print_findings(fs: list[dict]) -> None:
    if not fs:
        print("Không có gì bất thường trong log này.")
        return
    mark = {"alert": "!!", "warn": " !", "info": " -"}
    for f in fs:
        print(f"{mark[f['severity']]} {f['title']}")
        print(f"   {f['advice']}")


# --- the --ask path ----------------------------------------------------------

SYSTEM_PROMPT = """Bạn là trợ lý cho kỹ thuật viên âm thanh (soundman) dùng phần mềm chống hú \
AZ Soundtech Hands-free trong show trực tiếp. Bạn đọc log một buổi diễn qua các công cụ được \
cấp; bạn không điều khiển được phần mềm, mixer hay bất cứ thiết bị nào.

Cách trả lời:
- Tiếng Việt, ngắn, câu người; số đi kèm đơn vị (Hz, dB, giây).
- Mọi nhận định phải dựa trên dữ liệu công cụ trả về; nói rõ khi dữ liệu không đủ để kết luận.
- Bắt đầu từ findings() — đó là các phát hiện đã tính sẵn; dùng các công cụ khác để giải thích \
hoặc kiểm tra.
- Đây là âm thanh trực tiếp ra loa PA: không bao giờ khuyên tăng gain, tăng trần độ sâu notch, \
tắt chống hú, hay bỏ một giới hạn bảo vệ để "thử". Khi nghi ngờ, khuyên hạ gain và thử ở âm \
lượng nhỏ."""


def make_tools(events: list[dict]):
    """The read-only tool set for --ask, closed over one parsed log.

    Kept separate from the network call so a test can exercise every tool
    without credentials. Each returns a JSON string."""
    from anthropic import beta_tool

    s = logstats.summarise(events)
    fs = findings(events, s)

    @beta_tool
    def session_summary() -> str:
        """Session header: app version, device, sample rate, buffer, duration, modes, totals."""
        h = s["header"]
        return json.dumps({
            "app_version": h.get("app_version"), "device": h.get("device"),
            "sample_rate": h.get("sample_rate"), "buffer_size": h.get("buffer_size"),
            "duration_ms": s["duration_ms"], "modes": s["modes"],
            "notches": len(s["notches"]), "retunes": s["retunes"],
            "verdicts": s["verdicts"], "false": s["false"], "unjudged": s["unjudged"],
            "dropped_events": s["dropped"],
        }, ensure_ascii=False)

    @beta_tool
    def get_findings() -> str:
        """Precomputed findings (recurrence, ceiling, stacked, burst, false rate, ...), most severe first."""
        return json.dumps(fs, ensure_ascii=False)

    @beta_tool
    def list_notches(slot: int = -1) -> str:
        """Every notch placed in the session, in order.

        Args:
            slot: Only this slot; -1 for all slots.
        """
        rows = [dict(n, number=i) for i, n in enumerate(s["notches"], 1)
                if slot < 0 or n["slot"] == slot]
        return json.dumps(rows, ensure_ascii=False)

    @beta_tool
    def notch_context(number: int) -> str:
        """Detector evidence for one notch: score, peakiness and the spectrum bins around it.

        Args:
            number: The notch number from list_notches (1-based).
        """
        sets = [e for e in events if e.get("ev") == "notch_set"]
        if not 1 <= number <= len(sets):
            return json.dumps({"error": f"no notch {number}; there are {len(sets)}"})
        e = sets[number - 1]
        keep = ("t", "slot", "lane", "hz", "q", "depth_db", "origin", "score", "peakiness",
                "p_norm", "rise", "novelty", "penalty", "asymmetry", "thr", "ctx")
        return json.dumps({k: e[k] for k in keep if k in e}, ensure_ascii=False)

    return [session_summary, get_findings, list_notches, notch_context]


def ask(events: list[dict], question: str) -> int:
    try:
        import anthropic
    except ImportError:
        print("--ask cần gói `anthropic` (pip install anthropic).", file=sys.stderr)
        return 3

    client = anthropic.Anthropic()
    runner = client.beta.messages.tool_runner(
        model="claude-opus-5-5",
        max_tokens=16000,
        thinking={"type": "adaptive"},
        output_config={"effort": "medium"},
        # Refusal fallback, server side: a declined turn is re-run on a
        # fallback model inside the same call instead of simply stopping.
        betas=["server-side-fallback-2026-07-01"],
        fallbacks="default",
        system=SYSTEM_PROMPT,
        tools=make_tools(events),
        max_iterations=12,
        messages=[{"role": "user", "content": question}],
    )
    try:
        last = None
        for message in runner:
            last = message
    except (anthropic.AuthenticationError, TypeError):
        # TypeError: the SDK raises it, not an API error, when it finds no
        # credential source at all (no key, no token, no `ant` profile).
        print("Không có thông tin đăng nhập Claude API (ANTHROPIC_API_KEY hoặc `ant auth login`).",
              file=sys.stderr)
        return 3
    except anthropic.APIConnectionError as exc:
        print(f"Không kết nối được Claude API: {exc}", file=sys.stderr)
        return 3
    except anthropic.APIStatusError as exc:
        print(f"Claude API trả lỗi {exc.status_code}: {exc.message}", file=sys.stderr)
        return 3

    if last is None:
        return 3
    if last.stop_reason == "refusal":
        print("Claude từ chối trả lời câu này.", file=sys.stderr)
        return 3
    print("".join(b.text for b in last.content if b.type == "text"))
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path)
    ap.add_argument("--json", action="store_true", help="print findings as JSON")
    ap.add_argument("--ask", metavar="QUESTION", help="ask Claude about this session (network)")
    args = ap.parse_args(argv)
    # A Windows console defaults to a code page that cannot print Vietnamese.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    events = logstats.load(args.file)
    if not events:
        print("empty or unreadable log", file=sys.stderr)
        return 2
    fs = findings(events)
    if args.json:
        print(json.dumps(fs, ensure_ascii=False, indent=2))
    else:
        print_findings(fs)
    if args.ask:
        print()
        return ask(events, args.ask)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
