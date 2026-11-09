#!/usr/bin/env python3
"""Soundman copilot over a Hands-free session log (sidecar of the app; lanes L, L2a, L2b).

    python tools/copilot/copilot.py <session-*.jsonl>            # findings, no network
    python tools/copilot/copilot.py <file> --json                # same, machine-readable
    python tools/copilot/copilot.py <file> --ask "Vì sao 1 kHz hú hoài?"
    python tools/copilot/copilot.py <file> --ask Q --provider openai_compatible \
        --base-url http://localhost:11434/v1 --model llama3 [--timeout 120] [--json]
    echo KEY | python tools/copilot/copilot.py --store-key --provider anthropic
    python tools/copilot/copilot.py --has-key|--delete-key --provider anthropic

The findings are computed here, deterministically, from the log; they need no
network and no LLM. `--ask` hands the SAME data to a model through read-only
tools so a soundman can ask questions in plain words. The model can only read
what these tools return -- it cannot touch the app, the mixer, or the log.
Two adapters, never mixed: `anthropic` (official SDK + Tool Runner) and
`openai_compatible` (Chat Completions over urllib, stdlib only).

Exit codes (one scheme; argparse's own usage errors use 5 too, never its 2):
    0 ok | 1 --has-key: no stored key | 2 empty/unreadable log
    3 --ask failed (SDK/credentials/connection/HTTP/refusal/no reply/tool loop),
      a key-store operation failed, or any unexpected exception (type name only)
    4 answer blocked by the safety filter (the fixed sentence goes to stdout)
    5 bad arguments (unknown flag, missing --model/--base-url, bad URL, ...)

Never runs inside the app and never on the audio path: the app is a live-sound
insert, and a network call has no place in it (spec §2,
docs/superpowers/specs/2026-10-20-llm-copilot-design.md). API keys live only in
a DPAPI-encrypted per-user file and never reach stdout, stderr or logs.
"""
from __future__ import annotations

import argparse
import base64
import http.client
import ipaddress
import json
import math
import os
import re
import socket
import sys
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

if not getattr(sys, "frozen", False):
    # From source: logstats lives one level up (tools/). Frozen: PyInstaller
    # bundles it as a module (build_copilot.ps1 passes --paths tools).
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

MAX_RESPONSE_BYTES = 4 * 1024 * 1024   # cap on one HTTP response body
MAX_TOOL_RESULT_BYTES = 64 * 1024      # cap on one tool result handed to the model

EXIT_OK, EXIT_BAD_LOG, EXIT_ASK_FAILED, EXIT_BLOCKED, EXIT_BAD_ARGS = 0, 2, 3, 4, 5

SEVERITY_ORDER = {"alert": 0, "warn": 1, "info": 2}


def _lane(n: dict) -> str:
    return "R" if n.get("lane") == 1 else "L"


def _where(n: dict) -> str:
    # The log stores a zero-based slot index; the console labels slots 01, 02, ...
    slot = n.get("slot")
    label = f"{slot + 1:02d}" if isinstance(slot, int) else "?"
    return f"slot {label} làn {_lane(n)}"


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

PROVIDERS = ("anthropic", "openai_compatible")
DEFAULT_ANTHROPIC_MODEL = "claude-opus-5-5"
DEFAULT_TIMEOUT_S = 120.0
MAX_ITERATIONS = 12
# Server-side refusal fallback exists only for these model ids.
FALLBACK_MODELS = frozenset({"claude-fable-5-1", "claude-opus-5-5", "claude-opus-5",
                             "claude-sonnet-5-5"})
BLOCKED_MESSAGE = ("Câu trả lời của mô hình đã bị chặn vì nó khuyên một việc có thể gây hú. "
                   "Hãy hỏi lại, hoặc đổi mô hình.")


class AskError(Exception):
    """An --ask failure (exit 3). The message is already safe to print."""


# --- credentials (Windows DPAPI, per Windows user) ---------------------------

# The optional-entropy bytes passed to CryptProtectData / CryptUnprotectData.
# ONE constant: the C++ writer of copilot-credentials.json must use these exact
# bytes (documented in the L2b sidecar contract of the panel design spec).
DPAPI_ENTROPY = b"AZSoundtech.HandsFree.Copilot.v1"
CREDENTIALS_DIR_ENV = "HANDSFREE_COPILOT_CREDENTIALS_DIR"
CREDENTIALS_FILE = "copilot-credentials.json"
_CRYPTPROTECT_UI_FORBIDDEN = 0x1


class CredentialError(Exception):
    """A credential-store failure. Never carries a key."""


def credentials_path() -> Path:
    override = os.environ.get(CREDENTIALS_DIR_ENV)
    if override:
        base = Path(override)
    else:
        appdata = os.environ.get("APPDATA")
        if not appdata:
            raise CredentialError("APPDATA is not set; cannot locate the credentials file")
        base = Path(appdata) / "AZSoundtech" / "HandsFree"
    return base / CREDENTIALS_FILE


def _dpapi(protect: bool, data: bytes) -> bytes:
    if sys.platform != "win32":
        raise CredentialError("encrypted key storage needs Windows (DPAPI)")
    import ctypes
    from ctypes import wintypes

    class DataBlob(ctypes.Structure):
        _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

    def blob(b: bytes):
        buf = ctypes.create_string_buffer(b, len(b))
        return DataBlob(len(b), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))), buf

    crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    fn = crypt32.CryptProtectData if protect else crypt32.CryptUnprotectData
    fn.restype = wintypes.BOOL
    fn.argtypes = [ctypes.POINTER(DataBlob), ctypes.c_void_p if not protect else wintypes.LPCWSTR,
                   ctypes.POINTER(DataBlob), ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
                   ctypes.POINTER(DataBlob)]
    kernel32.LocalFree.restype = ctypes.c_void_p
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]

    src, src_buf = blob(data)
    ent, ent_buf = blob(DPAPI_ENTROPY)
    out = DataBlob()
    if not fn(ctypes.byref(src), None, ctypes.byref(ent), None, None,
              _CRYPTPROTECT_UI_FORBIDDEN, ctypes.byref(out)):
        raise CredentialError(f"DPAPI call failed (error {ctypes.get_last_error()})")
    try:
        return ctypes.string_at(out.pbData, out.cbData)
    finally:
        kernel32.LocalFree(ctypes.cast(out.pbData, ctypes.c_void_p))
        del src_buf, ent_buf


def _read_store(path: Path) -> dict:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {"version": 1, "profiles": {}}
    except (OSError, ValueError) as exc:
        raise CredentialError(f"credentials file unreadable: {type(exc).__name__}") from None
    if not isinstance(doc, dict) or not isinstance(doc.get("profiles"), dict):
        raise CredentialError("credentials file has an unexpected shape")
    return doc


def _write_store(path: Path, doc: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, path)


def store_key(provider: str, key: str) -> None:
    encrypted = _dpapi(True, key.encode("utf-8"))
    path = credentials_path()
    try:
        doc = _read_store(path)
    except CredentialError:
        # Never overwrite an unreadable file: it may hold other providers' profiles.
        corrupt = path.with_name(path.name + ".corrupt")
        try:
            os.replace(path, corrupt)
        except OSError as exc:
            raise CredentialError("cannot set the unreadable credentials file aside: "
                                  f"{type(exc).__name__}") from None
        print(f"File khoá cũ không đọc được; đã chuyển sang {corrupt.name}.", file=sys.stderr)
        doc = {"version": 1, "profiles": {}}
    doc["version"] = 1
    doc["profiles"][provider] = {"blob": base64.b64encode(encrypted).decode("ascii")}
    try:
        _write_store(path, doc)
    except OSError as exc:
        raise CredentialError(f"cannot write credentials file: {type(exc).__name__}") from None


def delete_key(provider: str) -> None:
    path = credentials_path()
    doc = _read_store(path)
    if doc["profiles"].pop(provider, None) is not None:
        try:
            _write_store(path, doc)
        except OSError as exc:
            raise CredentialError(f"cannot write credentials file: {type(exc).__name__}") from None


def load_key(provider: str) -> str | None:
    """The stored key for `provider`, or None. A corrupt entry is "no key" plus
    one stderr line (never the blob, never a key)."""
    if sys.platform != "win32":
        return None
    try:
        doc = _read_store(credentials_path())
        entry = doc["profiles"].get(provider)
        if entry is None:
            return None
        key = _dpapi(False, base64.b64decode(entry["blob"], validate=True)).decode("utf-8")
        if key:
            _SECRETS.add(key)
        return key or None
    except (CredentialError, KeyError, TypeError, ValueError, UnicodeDecodeError):
        print(f"Khoá đã lưu cho {provider} không đọc được; coi như chưa có khoá.",
              file=sys.stderr)
        return None


def resolve_key(provider: str) -> str | None:
    """Stored key first; then the provider's own environment fallback."""
    key = load_key(provider)
    if key:
        return key
    if provider == "openai_compatible":
        return os.environ.get("OPENAI_API_KEY") or None
    return None   # anthropic: the SDK resolves env var / `ant` profile itself


# Every key this process has read, plus the env vars an SDK may resolve by itself.
_SECRETS: set[str] = set()
_SECRET_ENV_VARS = ("ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "OPENAI_API_KEY")


def redact(text: str, key: str | None = None) -> str:
    """Replace `key`, every key read so far, and the env-provided key values."""
    secrets = set(_SECRETS)
    if key:
        secrets.add(key)
    for name in _SECRET_ENV_VARS:
        v = os.environ.get(name)
        if v:
            secrets.add(v)
    for sec in sorted(secrets, key=len, reverse=True):
        text = text.replace(sec, "***")
    return text


# --- shared read-only tools --------------------------------------------------

_NO_ARGS = {"type": "object", "properties": {}, "required": []}
TOOL_SPECS: dict[str, dict] = {
    "session_summary": {
        "description": "Session header: app version, device, sample rate, buffer, duration, modes, totals.",
        "parameters": _NO_ARGS},
    "get_findings": {
        "description": "Precomputed findings (recurrence, ceiling, stacked, burst, false rate, ...), most severe first.",
        "parameters": _NO_ARGS},
    "list_notches": {
        "description": "Every notch placed in the session, in order.",
        "parameters": {"type": "object", "required": [], "properties": {
            "slot": {"type": "integer", "description": "Only this slot; -1 for all slots."}}}},
    "notch_context": {
        "description": "Detector evidence for one notch: score, peakiness and the spectrum bins around it.",
        "parameters": {"type": "object", "required": ["number"], "properties": {
            "number": {"type": "integer", "description": "The notch number from list_notches (1-based)."}}}},
}


def make_tool_functions(events: list[dict]) -> dict:
    """Plain read-only tool functions closed over one parsed log. Each returns a
    JSON string. Nothing here writes anything."""
    s = logstats.summarise(events)
    fs = findings(events, s)

    def session_summary() -> str:
        h = s["header"]
        return json.dumps({
            "app_version": h.get("app_version"), "device": h.get("device"),
            "sample_rate": h.get("sample_rate"), "buffer_size": h.get("buffer_size"),
            "duration_ms": s["duration_ms"], "modes": s["modes"],
            "notches": len(s["notches"]), "retunes": s["retunes"],
            "verdicts": s["verdicts"], "false": s["false"], "unjudged": s["unjudged"],
            "dropped_events": s["dropped"],
        }, ensure_ascii=False)

    def get_findings() -> str:
        return json.dumps(fs, ensure_ascii=False)

    def list_notches(slot: int = -1) -> str:
        rows = [dict(n, number=i) for i, n in enumerate(s["notches"], 1)
                if slot < 0 or n["slot"] == slot]
        return json.dumps(rows, ensure_ascii=False)

    def notch_context(number: int) -> str:
        sets = [e for e in events if e.get("ev") == "notch_set"]
        if not 1 <= number <= len(sets):
            return json.dumps({"error": f"no notch {number}; there are {len(sets)}"})
        e = sets[number - 1]
        keep = ("t", "slot", "lane", "hz", "q", "depth_db", "origin", "score", "peakiness",
                "p_norm", "rise", "novelty", "penalty", "asymmetry", "thr", "ctx")
        return json.dumps({k: e[k] for k in keep if k in e}, ensure_ascii=False)

    return {"session_summary": session_summary, "get_findings": get_findings,
            "list_notches": list_notches, "notch_context": notch_context}


def _cap_result(result: str) -> str:
    """Keep a tool result under MAX_TOOL_RESULT_BYTES as VALID JSON: a list is
    cut to its longest fitting prefix and wrapped with a `truncated` marker."""
    if len(result.encode("utf-8")) <= MAX_TOOL_RESULT_BYTES:
        return result
    try:
        doc = json.loads(result)
    except ValueError:
        doc = None
    if not isinstance(doc, list):
        return json.dumps({"truncated": True, "error": "result exceeds 64 KiB"})
    total = len(doc)

    def wrap(items: list) -> str:
        return json.dumps({"truncated": True, "total": total, "items": items}, ensure_ascii=False)

    lo, hi = 0, total
    while lo < hi:   # largest k whose wrapped form fits
        mid = (lo + hi + 1) // 2
        if len(wrap(doc[:mid]).encode("utf-8")) <= MAX_TOOL_RESULT_BYTES:
            lo = mid
        else:
            hi = mid - 1
    return wrap(doc[:lo])


def run_tool(funcs: dict, name: object, raw_args: object) -> str:
    """Run one tool call from a model. Never raises: a bad name, bad arguments
    or any failure while running comes back as an error JSON string the model
    can read."""
    try:
        return _cap_result(_run_tool(funcs, name, raw_args))
    except Exception as exc:  # noqa: BLE001 -- model input must never abort the ask
        return json.dumps({"error": f"tool failed: {type(exc).__name__}"})


def _run_tool(funcs: dict, name: object, raw_args: object) -> str:
    if not isinstance(name, str) or name not in funcs:
        return json.dumps({"error": f"unknown tool {name!r}"}, ensure_ascii=False)
    if isinstance(raw_args, str):
        try:
            args = json.loads(raw_args) if raw_args.strip() else {}
        except (ValueError, RecursionError):
            return json.dumps({"error": "arguments are not valid JSON"})
    elif isinstance(raw_args, dict):
        args = raw_args
    elif raw_args is None:
        args = {}
    else:
        return json.dumps({"error": "arguments must be a JSON object"})
    if not isinstance(args, dict):
        return json.dumps({"error": "arguments must be a JSON object"})
    allowed = TOOL_SPECS[name]["parameters"]["properties"]
    clean = {}
    for k, v in args.items():
        if k not in allowed:
            return json.dumps({"error": f"unexpected argument {k!r}"})
        try:
            clean[k] = int(v)   # every parameter is an integer; models sometimes send "3"
        except (TypeError, ValueError, OverflowError):
            return json.dumps({"error": f"argument {k!r} must be an integer"})
    try:
        return funcs[name](**clean)
    except TypeError as exc:
        return json.dumps({"error": f"bad arguments: {exc}"})


def make_tools(events: list[dict]):
    """The same tools wrapped for the Anthropic Tool Runner (`beta_tool`)."""
    from anthropic import beta_tool

    f = make_tool_functions(events)

    @beta_tool
    def session_summary() -> str:
        """Session header: app version, device, sample rate, buffer, duration, modes, totals."""
        return _cap_result(f["session_summary"]())

    @beta_tool
    def get_findings() -> str:
        """Precomputed findings (recurrence, ceiling, stacked, burst, false rate, ...), most severe first."""
        return _cap_result(f["get_findings"]())

    @beta_tool
    def list_notches(slot: int = -1) -> str:
        """Every notch placed in the session, in order.

        Args:
            slot: Only this slot; -1 for all slots.
        """
        return _cap_result(f["list_notches"](slot))

    @beta_tool
    def notch_context(number: int) -> str:
        """Detector evidence for one notch: score, peakiness and the spectrum bins around it.

        Args:
            number: The notch number from list_notches (1-based).
        """
        return _cap_result(f["notch_context"](number))

    return [session_summary, get_findings, list_notches, notch_context]


# --- safety filter -----------------------------------------------------------

_TOKEN_RE = re.compile(r"[+\-]?\d+(?:[.,]\d+)?|[^\W_]+(?:'[^\W_]+)?|[,.;:!?]")
_NUMBER_RE = re.compile(r"[+\-]?\d+(?:[.,]\d+)?$")
_PUNCT = frozenset(",.;:!?")
_WINDOW = 4   # words allowed between an "increase" verb and its object

# Normalised forms (no diacritics, d for đ). Phrases are word tuples.
_BLOCKED_PHRASES = (("tat", "chong", "hu"), ("turn", "off", "feedback"),
                    ("disable", "feedback"), ("tat", "feedback"))
_NEGATIONS = frozenset({"dung", "khong", "tranh", "never", "not", "don't", "dont", "doesn't",
                        "shouldn't", "mustn't", "cannot", "can't", "avoid", "without"})
_NEGATION_FILLERS = frozenset({"nen"})   # "khong nen X", "dung nen X"
_VERBS = frozenset({"tang", "nang", "lift", "raise", "raises", "raised", "raising",
                    "increase", "increases", "increased", "increasing",
                    "boost", "boosts", "boosted", "boosting",
                    "crank", "cranks", "cranked", "cranking"})
_PARTICLE_VERBS = {"turn": "up", "push": "up", "bump": "up", "bring": "up",
                   "day": "len", "keo": "len", "van": "len"}
_TWO_WORD_VERBS = frozenset({("mo", "to"), ("mo", "lon")})
_AMOUNT_VERBS = frozenset({"cong", "them", "add"})   # "cong 3 dB", "them 3 dB"
_PARTICLES = frozenset({"up", "len"})
_OBJECTS = frozenset({"gain", "gains", "volume", "volumes", "fader", "faders", "level", "levels",
                      "muc", "trim", "preamp", "master", "output", "mic", "mics", "tran",
                      "ceiling", "monitor", "send", "db"})
_OBJECT_PAIRS = frozenset({("am", "luong"), ("do", "loi")})


def _fold(text: str, glue: bool) -> str:
    """NFKC, drop/split format characters, casefold, strip diacritics (đ -> d)."""
    t = unicodedata.normalize("NFKC", text)
    t = "".join(("" if glue else " ") if unicodedata.category(c) == "Cf" else c for c in t)
    t = t.casefold().replace("’", "'").replace("‘", "'").replace("ʼ", "'")
    t = t.replace("chớ", "đừng")   # "chớ" would fold to "cho" (= "for"), losing its negation
    t = "".join(c for c in unicodedata.normalize("NFD", t) if unicodedata.category(c) != "Mn")
    return " ".join(t.replace("đ", "d").split())


def _words(t: str) -> tuple[list[str], list[bool]]:
    """Word tokens, and for each whether a punctuation mark sits right before it."""
    words: list[str] = []
    after_punct: list[bool] = []
    pending = False
    for tok in _TOKEN_RE.findall(t):
        if tok in _PUNCT:
            pending = True
            continue
        words.append(tok)
        after_punct.append(pending)
        pending = False
    return words, after_punct


def _words_violate(w: list[str], pp: list[bool]) -> bool:
    n = len(w)

    def negated(i: int) -> bool:
        if i == 0 or pp[i]:
            return False
        if w[i - 1] in _NEGATIONS:
            return True
        return (w[i - 1] in _NEGATION_FILLERS and i >= 2 and not pp[i - 1]
                and w[i - 2] in _NEGATIONS)

    def obj_len(j: int) -> int:
        if j >= n:
            return 0
        if j + 1 < n and (w[j], w[j + 1]) in _OBJECT_PAIRS:
            return 2
        return 1 if w[j] in _OBJECTS else 0

    def object_between(lo: int, hi: int) -> bool:
        return any(obj_len(j) for j in range(lo, min(hi, n)))

    def object_just_before(i: int) -> bool:
        # An object up to 3 words before word i, no punctuation in between.
        for j in range(max(0, i - 3), i):
            if obj_len(j) and j + obj_len(j) <= i and not any(pp[j + 1:i + 1]):
                return True
        return False

    for i in range(n):
        for ph in _BLOCKED_PHRASES:
            if tuple(w[i:i + len(ph)]) == ph and not negated(i):
                return True
        if w[i][0] == "+" and _NUMBER_RE.match(w[i]) and i + 1 < n and w[i + 1] == "db" \
                and not negated(i):
            return True                                    # "+3 dB"
        if w[i] in _AMOUNT_VERBS and i + 2 < n and _NUMBER_RE.match(w[i + 1]) \
                and w[i + 2] == "db" and not negated(i):
            return True                                    # "cong 3 dB", "them 3 dB"
        if w[i] in _VERBS and not negated(i):
            if object_between(i + 1, i + 1 + _WINDOW) or object_just_before(i):
                return True
        if tuple(w[i:i + 2]) in _TWO_WORD_VERBS and not negated(i):
            if object_between(i + 2, i + 2 + _WINDOW):
                return True
        if w[i] in _PARTICLE_VERBS and not negated(i):
            part = _PARTICLE_VERBS[w[i]]
            k = next((x for x in range(i + 1, min(i + 1 + _WINDOW, n)) if w[x] == part), None)
            if k is not None:
                if w[i] == "turn" and k == i + 2 and w[i + 1] == "it":
                    return True                            # "turn it up"
                if object_between(i + 1, k + 1 + _WINDOW):
                    return True
        # "fader up", "gain len": the object first, the particle right after it.
        if obj_len(i) and w[i] != "db" and i + obj_len(i) < n and w[i + obj_len(i)] in _PARTICLES \
                and not pp[i + obj_len(i)] and not negated(i):
            return True
    return False


def violates_safety(text: str) -> bool:
    """True when `text` advises raising gain/ceiling or disabling protection.

    When in doubt, block: a false block costs one fixed sentence on screen, a
    false pass can reach a PA system. This is a pattern filter, not a guarantee:
    a model can still say it in words this list does not contain; the system
    prompt and the app's own clamps remain the other layers. Format characters
    are tried both removed and as spaces, so a zero-width split cannot hide a
    word either way."""
    return any(_words_violate(*_words(_fold(text, glue))) for glue in (True, False))


# --- anthropic adapter -------------------------------------------------------

def build_anthropic_kwargs(model: str, tools: list, question: str) -> dict:
    """The exact keyword arguments handed to `tool_runner` (pure; sends nothing)."""
    kwargs: dict = {
        "model": model,
        "max_tokens": 16000,
        "thinking": {"type": "adaptive"},
        "output_config": {"effort": "medium"},
        "system": SYSTEM_PROMPT,
        "tools": tools,
        "max_iterations": MAX_ITERATIONS,
        "messages": [{"role": "user", "content": question}],
    }
    if model in FALLBACK_MODELS:
        # Refusal fallback, server side: a declined turn is re-run on a
        # fallback model inside the same call instead of simply stopping.
        kwargs["betas"] = ["server-side-fallback-2026-07-01"]
        kwargs["fallbacks"] = "default"
    return kwargs


def _sum_usage(values: list) -> int | None:
    nums = [v for v in values if isinstance(v, int) and not isinstance(v, bool)]
    return sum(nums) if nums else None


def run_anthropic(events: list[dict], question: str, model: str, timeout: float) -> dict:
    try:
        import anthropic
    except ImportError:
        raise AskError("--ask cần gói `anthropic` (pip install anthropic).") from None

    key = resolve_key("anthropic")
    try:
        client = anthropic.Anthropic(api_key=key, timeout=timeout) if key \
            else anthropic.Anthropic(timeout=timeout)
        runner = client.beta.messages.tool_runner(
            **build_anthropic_kwargs(model, make_tools(events), question))
        last = None
        inp: list = []
        outp: list = []
        for message in runner:
            last = message
            u = getattr(message, "usage", None)
            inp.append(getattr(u, "input_tokens", None))
            outp.append(getattr(u, "output_tokens", None))
    except (anthropic.AuthenticationError, TypeError):
        # TypeError: the SDK raises it, not an API error, when it finds no
        # credential source at all (no key, no token, no `ant` profile).
        raise AskError("Không có thông tin đăng nhập Claude API (lưu khoá trong app, "
                       "ANTHROPIC_API_KEY, hoặc `ant auth login`).") from None
    except anthropic.APIConnectionError as exc:
        raise AskError(redact(f"Không kết nối được Claude API: {exc}", key)) from None
    except anthropic.APIStatusError as exc:
        raise AskError(redact(f"Claude API trả lỗi {exc.status_code}: {exc.message}", key)) from None

    if last is None:
        raise AskError("Claude không trả lời.")
    if last.stop_reason == "refusal":
        raise AskError("Claude từ chối trả lời câu này.")
    answer = "".join(b.text for b in last.content if b.type == "text")
    if not answer.strip():
        raise AskError("Claude không trả lời.")
    return {"answer": answer, "usage": {"input_tokens": _sum_usage(inp),
                                        "output_tokens": _sum_usage(outp)}}


# --- openai_compatible adapter (stdlib only) ---------------------------------

class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Never follow a redirect: the Authorization header must not travel to
    another host. A 3xx surfaces as an HTTPError."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D401
        return None


def _openai_tools() -> list[dict]:
    return [{"type": "function",
             "function": {"name": n, "description": sp["description"], "parameters": sp["parameters"]}}
            for n, sp in TOOL_SPECS.items()]


def _post_json(url: str, payload: dict, key: str | None, timeout: float) -> dict:
    headers = {"Content-Type": "application/json", "Accept": "application/json"}
    if key:
        headers["Authorization"] = f"Bearer {key}"
    req = urllib.request.Request(url, data=json.dumps(payload).encode("utf-8"),
                                 headers=headers, method="POST")
    opener = urllib.request.build_opener(_NoRedirect)
    try:
        with opener.open(req, timeout=timeout) as resp:
            body = resp.read(MAX_RESPONSE_BYTES + 1)
    except urllib.error.HTTPError as exc:
        try:
            excerpt = exc.read(2000).decode("utf-8", "replace")
        except Exception:  # noqa: BLE001
            excerpt = ""
        finally:
            exc.close()
        excerpt = " ".join(redact(excerpt, key).split())[:300]
        kind = "chuyển hướng (không theo)" if 300 <= exc.code < 400 else "lỗi"
        raise AskError(f"Máy chủ trả {kind} {exc.code}: {excerpt}") from None
    except (TimeoutError, socket.timeout):
        raise AskError(f"Quá hạn {timeout:g} s chờ máy chủ.") from None
    except urllib.error.URLError as exc:
        if isinstance(exc.reason, (TimeoutError, socket.timeout)):
            raise AskError(f"Quá hạn {timeout:g} s chờ máy chủ.") from None
        raise AskError(redact(f"Không kết nối được máy chủ: {exc.reason}", key)) from None
    except (OSError, http.client.HTTPException) as exc:
        raise AskError(redact(f"Lỗi kết nối: {type(exc).__name__}", key)) from None
    if len(body) > MAX_RESPONSE_BYTES:
        raise AskError("Máy chủ trả dữ liệu quá lớn (hơn 4 MiB).")
    try:
        doc = json.loads(body.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        raise AskError("Máy chủ trả dữ liệu không phải JSON.") from None
    if not isinstance(doc, dict):
        raise AskError("Máy chủ trả JSON không đúng dạng.")
    return doc


def _message_text(content: object) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(p.get("text", "") for p in content
                       if isinstance(p, dict) and isinstance(p.get("text"), str))
    return ""


def _is_loopback(host: str | None) -> bool:
    if not host:
        return False
    if host.lower() == "localhost":
        return True
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        return False
    if getattr(ip, "ipv4_mapped", None) is not None:
        ip = ip.ipv4_mapped
    return ip.is_loopback


def _refuse_plaintext_key(base_url: str, key: str | None) -> None:
    """A key must never travel over plain http:// to a non-loopback host."""
    parts = urllib.parse.urlsplit(base_url)
    if key and parts.scheme == "http" and not _is_loopback(parts.hostname):
        raise AskError("Từ chối gửi khoá API qua http:// không mã hoá tới máy chủ ngoài máy này; "
                       "dùng https:// hoặc xoá khoá đã lưu.")


def run_openai_compatible(events: list[dict], question: str, model: str, base_url: str,
                          key: str | None, timeout: float) -> dict:
    _refuse_plaintext_key(base_url, key)
    url = base_url.rstrip("/") + "/chat/completions"
    funcs = make_tool_functions(events)
    tools = _openai_tools()
    messages: list[dict] = [{"role": "system", "content": SYSTEM_PROMPT},
                            {"role": "user", "content": question}]
    tokens_in: list = []
    tokens_out: list = []
    for _ in range(MAX_ITERATIONS):
        doc = _post_json(url, {"model": model, "messages": messages, "tools": tools,
                               "tool_choice": "auto"}, key, timeout)
        usage = doc.get("usage") if isinstance(doc.get("usage"), dict) else {}
        tokens_in.append(usage.get("prompt_tokens"))
        tokens_out.append(usage.get("completion_tokens"))
        choices = doc.get("choices")
        if not isinstance(choices, list) or not choices or not isinstance(choices[0], dict):
            raise AskError("Máy chủ trả về không có `choices`.")
        choice = choices[0]
        if choice.get("finish_reason") == "content_filter":
            raise AskError("Nhà cung cấp từ chối trả lời (content_filter).")
        msg = choice.get("message")
        if not isinstance(msg, dict):
            raise AskError("Máy chủ trả về không có `message`.")
        calls = msg.get("tool_calls")
        if isinstance(calls, list) and calls:
            messages.append({"role": "assistant", "content": msg.get("content"),
                             "tool_calls": calls})
            for i, call in enumerate(calls):
                call = call if isinstance(call, dict) else {}
                fn = call.get("function") if isinstance(call.get("function"), dict) else {}
                result = run_tool(funcs, fn.get("name"), fn.get("arguments"))
                messages.append({"role": "tool", "tool_call_id": str(call.get("id", f"call_{i}")),
                                 "content": result})
            continue
        answer = _message_text(msg.get("content"))
        if not answer.strip():
            raise AskError("Mô hình không trả lời.")
        return {"answer": answer, "usage": {"input_tokens": _sum_usage(tokens_in),
                                            "output_tokens": _sum_usage(tokens_out)}}
    raise AskError("tool loop exhausted: mô hình gọi công cụ quá 12 lượt mà chưa trả lời.")


# --- argument validation -----------------------------------------------------

def validate_ask_args(provider: str, model: str | None, base_url: str | None,
                      timeout: float) -> str | None:
    """An error message for a bad --ask flag combination, else None."""
    if timeout <= 0:
        return "--timeout must be greater than 0"
    if provider == "anthropic":
        if base_url:
            return "--base-url is only for --provider openai_compatible"
        return None
    if not model:
        return "--model is required for --provider openai_compatible"
    if not base_url:
        return "--base-url is required for --provider openai_compatible"
    parts = urllib.parse.urlsplit(base_url)
    if parts.scheme not in ("http", "https") or not parts.netloc:
        return "--base-url must be an http:// or https:// URL"
    return None


def ask(events: list[dict], question: str, provider: str, model: str,
        base_url: str | None, timeout: float) -> dict:
    """Run one question. Returns {answer, usage}; raises AskError."""
    if provider == "anthropic":
        return run_anthropic(events, question, model, timeout)
    return run_openai_compatible(events, question, model, base_url or "",
                                 resolve_key("openai_compatible"), timeout)


def _force_utf8() -> None:
    # A frozen Python writing to a pipe on Windows uses the ANSI code page,
    # which cannot carry Vietnamese. The app reads this stdout as UTF-8.
    for stream in (sys.stdout, sys.stderr, sys.stdin):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass


def _lower_priority() -> None:
    """BELOW_NORMAL priority class on Windows; any failure is ignored."""
    if sys.platform != "win32":
        return
    try:
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(
            ctypes.windll.kernel32.GetCurrentProcess(), 0x00004000)
    except Exception:  # noqa: BLE001 -- best effort, must never change output
        pass


class _Parser(argparse.ArgumentParser):
    def error(self, message):  # argparse's default exit code 2 would collide with "empty log"
        self.print_usage(sys.stderr)
        print(f"{self.prog}: error: {message}", file=sys.stderr)
        raise SystemExit(EXIT_BAD_ARGS)


def _key_operation(args) -> int:
    try:
        if args.store_key:
            try:
                key = sys.stdin.readline().strip()
            except (UnicodeError, OSError) as exc:
                print(f"Lỗi kho khoá: không đọc được khoá từ stdin ({type(exc).__name__})",
                      file=sys.stderr)
                return EXIT_ASK_FAILED
            if not key:
                print("no key on stdin", file=sys.stderr)
                return EXIT_BAD_ARGS
            _SECRETS.add(key)
            store_key(args.provider, key)
            print(f"Đã lưu khoá cho {args.provider}.")
            return EXIT_OK
        if args.delete_key:
            delete_key(args.provider)
            print(f"Đã xoá khoá của {args.provider}.")
            return EXIT_OK
        return EXIT_OK if load_key(args.provider) else 1
    except CredentialError as exc:
        print(redact(f"Lỗi kho khoá: {exc}"), file=sys.stderr)
        return EXIT_ASK_FAILED
    except OSError as exc:
        print(f"Lỗi kho khoá: {type(exc).__name__}", file=sys.stderr)
        return EXIT_ASK_FAILED


def _emit_error(args, message: str) -> int:
    """An --ask failure: the redacted reason on stderr, and with --json the
    same reason as the one JSON object on stdout."""
    message = redact(message)
    print(message, file=sys.stderr)
    if args.json:
        print(json.dumps({"error": message}, ensure_ascii=False))
    return EXIT_ASK_FAILED


def main(argv: list[str]) -> int:
    """Exit 1 is reachable only from --has-key: anything unexpected is exit 3."""
    try:
        return _main(argv)
    except Exception as exc:  # noqa: BLE001 -- SystemExit/KeyboardInterrupt are not Exceptions
        name = type(exc).__name__   # the type only: str(exc) could carry a key or a path
        print(f"Lỗi không lường trước: {name}", file=sys.stderr)
        if "--json" in argv and any(a == "--ask" or a.startswith("--ask=") for a in argv):
            print(json.dumps({"error": f"Lỗi không lường trước trong trợ lý ({name})"},
                             ensure_ascii=False))
        return EXIT_ASK_FAILED


def _main(argv: list[str]) -> int:
    _force_utf8()
    ap = _Parser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path, nargs="?")
    ap.add_argument("--json", action="store_true", help="print findings as JSON")
    ap.add_argument("--ask", metavar="QUESTION", help="ask an AI model about this session (network)")
    ap.add_argument("--provider", choices=PROVIDERS, default="anthropic")
    ap.add_argument("--model", metavar="ID")
    ap.add_argument("--base-url", metavar="URL")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S, metavar="SECONDS")
    ap.add_argument("--store-key", action="store_true",
                    help="read an API key from stdin and store it (DPAPI)")
    ap.add_argument("--delete-key", action="store_true")
    ap.add_argument("--has-key", action="store_true",
                    help="exit 0 if a key is stored for --provider, else 1")
    ap.add_argument("--low-priority", action="store_true",
                    help="lower the process priority (Windows); output is unchanged")
    args = ap.parse_args(argv)
    if args.low_priority:
        _lower_priority()

    key_ops = sum((args.store_key, args.delete_key, args.has_key))
    if key_ops:
        if key_ops > 1 or args.ask is not None:
            ap.error("--store-key, --delete-key, --has-key and --ask are mutually exclusive")
        return _key_operation(args)
    if args.file is None:
        ap.error("the log file is required")

    model = args.model
    if args.ask is not None:
        if args.provider == "anthropic" and not model:
            model = DEFAULT_ANTHROPIC_MODEL
        problem = validate_ask_args(args.provider, model, args.base_url, args.timeout)
        if problem:
            ap.error(problem)
    elif args.model or args.base_url:
        ap.error("--model and --base-url only apply with --ask")

    try:
        events = logstats.load(args.file)
        if any(not isinstance(e, dict) for e in events):
            raise ValueError("a log line is not a JSON object")
        fs = findings(events) if events else []
    except OSError as exc:
        print(f"empty or unreadable log: {exc}", file=sys.stderr)
        return EXIT_BAD_LOG
    except Exception as exc:  # noqa: BLE001 -- undecodable / malformed log: exit 2, type only
        print(f"unreadable log: {type(exc).__name__}", file=sys.stderr)
        return EXIT_BAD_LOG
    if not events:
        print("empty or unreadable log", file=sys.stderr)
        return EXIT_BAD_LOG

    if args.ask is None:
        if args.json:
            print(json.dumps(fs, ensure_ascii=False, indent=2))
        else:
            print_findings(fs)
        return EXIT_OK

    if not args.json:
        print_findings(fs)
        print()
    try:
        result = ask(events, args.ask, args.provider, model, args.base_url, args.timeout)
    except AskError as exc:
        return _emit_error(args, str(exc))
    except Exception as exc:  # noqa: BLE001 -- last resort; type name only, never the message
        return _emit_error(args, f"Lỗi không lường trước trong trợ lý ({type(exc).__name__})")
    if violates_safety(result["answer"]):
        # The blocked answer itself is never printed, in any mode.
        if args.json:
            print(json.dumps({"blocked": True, "message": BLOCKED_MESSAGE}, ensure_ascii=False))
        else:
            print(BLOCKED_MESSAGE)
        return EXIT_BLOCKED
    if args.json:
        print(json.dumps({"answer": result["answer"], "blocked": False,
                          "provider": args.provider, "model": model,
                          "usage": result["usage"], "findings": fs}, ensure_ascii=False))
    else:
        print(result["answer"])
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
