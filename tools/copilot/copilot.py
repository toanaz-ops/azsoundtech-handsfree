#!/usr/bin/env python3
"""Soundman copilot over a Hands-free session log (sidecar of the app; lanes L, L2a, L2b).

    python tools/copilot/copilot.py <session-*.jsonl>            # findings, no network
    python tools/copilot/copilot.py <file> --json                # same, machine-readable
    python tools/copilot/copilot.py <file> --ask "Vì sao 1 kHz hú hoài?"
    python tools/copilot/copilot.py <file> --ask Q --provider openai_compatible \
        --base-url http://localhost:11434/v1 --model llama3 [--timeout 120] [--json]
    echo KEY | python tools/copilot/copilot.py --store-key --provider anthropic
    echo KEY | python tools/copilot/copilot.py --store-key --provider openai_compatible \
        --base-url http://localhost:11434/v1     # the key is bound to this origin
    python tools/copilot/copilot.py --has-key|--delete-key --provider anthropic
    python tools/copilot/copilot.py --has-key --provider openai_compatible --base-url URL
    ... --ask Q --credentials-only   # only the stored key; never an environment key
    python tools/copilot/copilot.py <file> --supervise --state=<state.json> --json \
        [--provider P --model M --base-url U --timeout S --credentials-only --low-priority]

The findings are computed here, deterministically, from the log; they need no
network and no LLM. `--ask` hands the SAME data to a model through read-only
tools so a soundman can ask questions in plain words. The model can only read
what these tools return -- it cannot touch the app, the mixer, or the log.
Two adapters, never mixed: `anthropic` (official SDK + Tool Runner) and
`openai_compatible` (Chat Completions over urllib, stdlib only).

`--supervise` (lane L3a) is the machine-to-machine mode: the app writes a state
file (schema 1, validated strictly) and the model may only call recording tools
(propose_deepen / propose_hold / propose_release / no_action); those calls ARE
the output. stdout is exactly one JSON object
    {"supervise": true, "generation": N, "actions": [...], "provider": P,
     "model": M, "usage": {"input_tokens": n|null, "output_tokens": n|null}}
and every word the model writes is discarded: no free text reaches stdout and
the safety filter plays no part. The C++ app re-checks every action; this side
is only the first line of defence. Failures print {"error": "..."} on stdout.

Exit codes (one scheme; argparse's own usage errors use 5 too, never its 2):
    0 ok | 1 --has-key: no stored key | 2 empty/unreadable log (--supervise:
      also a missing or invalid state file)
    3 --ask/--supervise failed (SDK/credentials/connection/HTTP/refusal/no reply/
      tool loop), a key-store operation failed, or any unexpected exception
      (type name only)
    4 answer blocked by the safety filter (--ask only; the fixed sentence goes to stdout)
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
import time
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


def _by_detector(n: dict) -> bool:
    """A notch the detector placed by itself. Log schema 4 adds `author`; a record
    without one (schema 1-3) is detector-authored when its origin is `detector`,
    exactly as before (logstats fills the effective author in)."""
    return n["origin"] == "detector" and n.get("author", "detector") == "detector"


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
        # Log schema 4: only depth the detector reached by itself counts; a
        # notch another author placed, or a depth another author retuned to, is
        # not the loop-gain evidence this rule is about.
        deepest = n.get("detector_deepest_db", n["deepest_db"])
        if not _by_detector(n) or deepest is None or n["verdict"] == "false":
            continue
        ceil = ceilings.get(int(n["slot"] or 0), ceilings.get(-1))
        if ceil is not None and float(deepest) <= ceil + CEILING_SLACK_DB:
            out.append({
                "code": "ceiling", "severity": "alert",
                "title": f"{n['hz']:.0f} Hz chạm trần {ceil:.0f} dB ({_where(n)})",
                "advice": ("Notch đã sâu tới mức tối đa cho phép: độ lợi vòng ở tần số này cao. "
                           "Hạ gain monitor hoặc kênh đó vài dB; đừng tăng trần."),
                "evidence": {"hz": round(n["hz"], 1), "slot": n["slot"], "lane": _lane(n),
                             "deepest_db": deepest, "ceiling_db": ceil},
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
    det_t = sorted(n["set_t"] for n in notches if _by_detector(n))
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


class LoopStopped(AskError):
    """The tool loop stopped before the model finished: the supervise deadline
    or the iteration cap. `supervise` still prints the actions already recorded."""

    def __init__(self, message: str, usage: dict | None = None):
        super().__init__(message)
        self.usage = usage or {"input_tokens": None, "output_tokens": None}


class ProviderUnavailable(LoopStopped):
    """A provider request timed out or the connection failed (not an HTTP error
    status, not a refusal). `supervise` still prints the actions already recorded."""


def _now() -> float:
    """The monotonic clock of the supervise deadline (tests replace it)."""
    return time.monotonic()


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


def origin_of(base_url: str | None) -> str | None:
    """The origin a stored openai_compatible key is bound to, or None.

    lowercase scheme + "://" + lowercase host (IPv6 keeps its brackets) + ":port"
    only when the port is given and differs from the scheme default (http 80,
    https 443); path, query and fragment are dropped. The C++ side implements
    the same function; the shared test vectors live in tests/test_copilot.py."""
    try:
        parts = urllib.parse.urlsplit(base_url or "")
        scheme = parts.scheme.lower()
        host = (parts.hostname or "").lower()
        port = parts.port
    except ValueError:
        return None
    if scheme not in ("http", "https") or not host:
        return None
    if "@" in parts.netloc:      # userinfo: never bind a key to such a URL
        return None
    if ":" in host:
        host = f"[{host}]"
    default = 80 if scheme == "http" else 443
    return f"{scheme}://{host}" + (f":{port}" if port is not None and port != default else "")


def store_key(provider: str, key: str, origin: str | None = None) -> None:
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
    entry = {"blob": base64.b64encode(encrypted).decode("ascii")}
    if origin:
        entry["origin"] = origin
    doc["profiles"][provider] = entry
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


def load_key(provider: str, base_url: str | None = None) -> str | None:
    """The stored key for `provider`, or None. A corrupt entry is "no key" plus
    one stderr line (never the blob, never a key). An openai_compatible key is
    bound to the origin it was stored for: it is returned only when that origin
    equals the origin of `base_url`; an entry with no origin (release 1.5.0) or a
    different one is "no key" plus one stderr line."""
    if sys.platform != "win32":
        return None
    try:
        doc = _read_store(credentials_path())
        entry = doc["profiles"].get(provider)
        if entry is None:
            return None
        if provider == "openai_compatible":
            want = origin_of(base_url)
            if not want or entry.get("origin") != want:
                print("Khoá đã lưu cho openai_compatible không gắn với máy chủ này; "
                      "coi như chưa có khoá.", file=sys.stderr)
                return None
        key = _dpapi(False, base64.b64decode(entry["blob"], validate=True)).decode("utf-8")
        if key:
            _SECRETS.add(key)
        return key or None
    except (CredentialError, KeyError, TypeError, AttributeError, ValueError, UnicodeDecodeError):
        print(f"Khoá đã lưu cho {provider} không đọc được; coi như chưa có khoá.",
              file=sys.stderr)
        return None


def resolve_key(provider: str, base_url: str | None = None,
                credentials_only: bool = False) -> str | None:
    """Stored key first; then the provider's own environment fallback, unless
    `credentials_only` (the app always passes it: it never sends a key it did not store)."""
    key = load_key(provider, base_url)
    if key or credentials_only:
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


def run_tool(funcs: dict, name: object, raw_args: object, specs: dict | None = None) -> str:
    """Run one tool call from a model. Never raises: a bad name, bad arguments
    or any failure while running comes back as an error JSON string the model
    can read. `specs` is the tool table of the mode (default: the --ask table)."""
    try:
        return _cap_result(_run_tool(funcs, name, raw_args, TOOL_SPECS if specs is None else specs))
    except Exception as exc:  # noqa: BLE001 -- model input must never abort the ask
        return json.dumps({"error": f"tool failed: {type(exc).__name__}"})


def _run_tool(funcs: dict, name: object, raw_args: object, specs: dict) -> str:
    if not isinstance(name, str) or name not in funcs or name not in specs:
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
    if specs[name].get("raw"):
        # A recording tool validates its own arguments strictly (no coercion:
        # "3" is not an integer here); it gets the dict exactly as sent.
        return funcs[name](args)
    allowed = specs[name]["parameters"]["properties"]
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


# --- tool sets and the --supervise mode (lane L3a) ---------------------------

class ToolSet:
    """The tools, their schemas and the system prompt of one mode. `--ask` uses
    the module-level read-only table; `--supervise` builds its own (below).
    `allow_empty_reply`: the model may finish with no text (supervise: its
    recorded calls are the whole output, any text is discarded anyway)."""

    def __init__(self, system: str, specs: dict, funcs: dict, allow_empty_reply: bool = False,
                 supervisor: "Supervisor | None" = None, max_iterations: int = MAX_ITERATIONS,
                 deadline_at: float | None = None):
        self.system = system
        self.specs = specs
        self.funcs = funcs
        self.allow_empty_reply = allow_empty_reply
        self.supervisor = supervisor
        self.max_iterations = max_iterations
        self.deadline_at = deadline_at      # absolute _now() time; None = no deadline

    def request_timeout(self, timeout: float) -> float:
        """The timeout for the next provider request: `timeout`, shrunk to the
        time left. Raises LoopStopped when less than SUPERVISE_MIN_REMAINING_S is left."""
        if self.deadline_at is None:
            return timeout
        remaining = self.deadline_at - _now()
        if remaining < SUPERVISE_MIN_REMAINING_S:
            raise LoopStopped("deadline: the supervise time budget is spent")
        return min(timeout, remaining)


def make_toolset_tools(ts: ToolSet) -> list:
    """Any ToolSet wrapped for the Anthropic Tool Runner. The schema is the one
    in `ts.specs` verbatim (no inference from a signature) and every call goes
    through `run_tool`, so the validation is the same code on both adapters."""
    from anthropic import beta_tool

    def wrap(name: str):
        def call(**kwargs: object) -> str:
            return run_tool(ts.funcs, name, kwargs, ts.specs)
        call.__name__ = name
        return call

    return [beta_tool(wrap(n), name=n, description=sp["description"], input_schema=sp["parameters"])
            for n, sp in ts.specs.items()]


EXIT_BAD_STATE = EXIT_BAD_LOG   # an unreadable or invalid state file is bad input, like a bad log

SUPERVISE_MAX_ACTIONS = 8
SUPERVISE_MAX_SLOTS = 16
SUPERVISE_MAX_NOTCHES = 32
MAX_STATE_BYTES = 1024 * 1024
STATE_SCHEMA = 2
# --supervise runs under a wall-clock deadline. The app passes --timeout=30
# --deadline-s=90 and kills the process at 105 s.
SUPERVISE_MAX_ITERATIONS = 6
SUPERVISE_DEADLINE_DEFAULT_S = 90
SUPERVISE_DEADLINE_MIN_S, SUPERVISE_DEADLINE_MAX_S = 10, 600
SUPERVISE_MIN_REMAINING_S = 5.0       # no new provider request with less than this left
RUNG_TOLERANCE_DB = 0.001
HOLD_S_MIN, HOLD_S_MAX = 30, 600

AI_MODES = ("suggest", "act")
ENGINE_MODES = ("auto", "soundcheck", "bypass")
TRIGGERS = ("recurring_bin", "at_ceiling", "heartbeat", "manual")
RING_RISKS = ("na", "low", "rising", "critical")
ORIGINS = ("detector", "preset", "manual", "soundcheck")
VERDICTS = ("none", "good", "false")
DEEPEN_REASONS = ("recurring_at_frequency", "still_ringing_at_depth")
HOLD_REASONS = ("recurring_at_frequency", "show_in_progress")
RELEASE_REASONS = ("judged_false", "stale_no_recurrence")
NO_ACTION_REASONS = ("nothing_to_do", "insufficient_data", "outside_scope")
_AUTHOR_RE = re.compile(r"[A-Za-z0-9_]{1,32}\Z")

# Fixed strings handed back to the model by a recording tool. Never built from
# model input, so nothing the model wrote can echo anywhere.
RESULT_QUEUED = "queued"
RESULT_LIMIT = "limit reached"
RESULT_DUPLICATE = "duplicate ignored"


class StateError(Exception):
    """The state file is missing or invalid (exit 2). The message is fixed text."""


def _is_int(v: object) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _is_num(v: object) -> bool:
    """A finite real number: not a bool, not NaN / Infinity, not an int too big for a float."""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return False
    try:
        return math.isfinite(v)
    except OverflowError:
        return False


def _reject_json_constant(_name: str):
    raise ValueError("bare non-finite constant")


def _state_keys(doc: object, keys: tuple, what: str) -> dict:
    if not isinstance(doc, dict):
        raise StateError(f"state: {what} must be an object")
    if set(doc) != set(keys):
        extra, missing = sorted(set(doc) - set(keys)), sorted(set(keys) - set(doc))
        raise StateError(f"state: {what} has "
                         + (f"unknown keys {extra}" if extra else f"missing keys {missing}"))
    return doc


def _state_int(d: dict, key: str, lo: int, hi: int, what: str) -> int:
    v = d[key]
    if not _is_int(v) or not lo <= v <= hi:
        raise StateError(f"state: {what}.{key} must be an integer in {lo}..{hi}")
    return v


def _state_num(d: dict, key: str, lo: float, hi: float, what: str) -> float:
    v = d[key]
    if not _is_num(v) or not lo <= v <= hi:
        raise StateError(f"state: {what}.{key} must be a finite number in {lo:g}..{hi:g}")
    return v


def _state_enum(d: dict, key: str, allowed: tuple, what: str) -> str:
    v = d[key]
    if not isinstance(v, str) or v not in allowed:
        raise StateError(f"state: {what}.{key} must be one of {list(allowed)}")
    return v


def validate_state(doc: object) -> dict:
    """Strict check of the app's state file; returns it. Anything unexpected is
    a StateError: unknown or missing keys, a wrong type (a bool is not a number),
    NaN / Infinity, a value out of range, more than 16 slots or 32 notches per
    slot, a repeated slot or notch id."""
    top = _state_keys(doc, ("schema", "generation", "ai_mode", "engine_mode", "sample_rate",
                            "bin_hz", "trigger", "slots"), "top level")
    if not _is_int(top["schema"]) or top["schema"] != STATE_SCHEMA:
        raise StateError(f"state: schema must be {STATE_SCHEMA}")
    _state_int(top, "generation", 0, 2 ** 53, "top level")
    _state_enum(top, "ai_mode", AI_MODES, "top level")
    _state_enum(top, "engine_mode", ENGINE_MODES, "top level")
    _state_num(top, "sample_rate", 1.0, 1e6, "top level")
    _state_num(top, "bin_hz", 1e-3, 1e5, "top level")
    _state_enum(top, "trigger", TRIGGERS, "top level")
    slots = top["slots"]
    if not isinstance(slots, list) or len(slots) > SUPERVISE_MAX_SLOTS:
        raise StateError(f"state: slots must be a list of at most {SUPERVISE_MAX_SLOTS}")
    seen_slots: set[int] = set()
    seen_nids: set[int] = set()
    for s in slots:
        s = _state_keys(s, ("slot", "ring_risk", "ring_risk_score", "notches"), "a slot")
        idx = _state_int(s, "slot", 0, SUPERVISE_MAX_SLOTS - 1, "slot")
        if idx in seen_slots:
            raise StateError("state: a slot index is repeated")
        seen_slots.add(idx)
        _state_enum(s, "ring_risk", RING_RISKS, "slot")
        if s["ring_risk_score"] is not None:
            _state_num(s, "ring_risk_score", 0.0, 1e6, "slot")
        notches = s["notches"]
        if not isinstance(notches, list) or len(notches) > SUPERVISE_MAX_NOTCHES:
            raise StateError(f"state: notches must be a list of at most {SUPERVISE_MAX_NOTCHES}")
        for n in notches:
            n = _state_keys(n, ("nid", "lane", "index", "hz", "q", "depth_db", "ceiling_db",
                                "next_rung_db", "origin", "placed_by", "last_changed_by", "seen_s",
                                "hold_s", "verdict"), "a notch")
            nid = _state_int(n, "nid", 0, 2 ** 32 - 1, "notch")
            if nid in seen_nids:
                raise StateError("state: a notch id is repeated")
            seen_nids.add(nid)
            _state_int(n, "lane", 0, 1, "notch")
            _state_int(n, "index", 0, 63, "notch")
            _state_num(n, "hz", 1e-3, top["sample_rate"] / 2.0, "notch")
            _state_num(n, "q", 1e-3, 1e4, "notch")
            _state_num(n, "depth_db", -120.0, 0.0, "notch")
            _state_num(n, "ceiling_db", -120.0, 0.0, "notch")
            if n["next_rung_db"] is not None:
                _state_num(n, "next_rung_db", -120.0, 0.0, "notch")
            _state_enum(n, "origin", ORIGINS, "notch")
            for k in ("placed_by", "last_changed_by"):
                if not isinstance(n[k], str) or not _AUTHOR_RE.match(n[k]):
                    raise StateError(f"state: notch.{k} must be a short identifier string")
            _state_num(n, "seen_s", 0.0, 1e8, "notch")
            _state_num(n, "hold_s", 0.0, 1e8, "notch")
            _state_enum(n, "verdict", VERDICTS, "notch")
    return doc


def load_state(path: Path) -> dict:
    """Read and validate the state file (UTF-8 JSON). Raises StateError."""
    try:
        with open(path, "rb") as fh:
            raw = fh.read(MAX_STATE_BYTES + 1)
    except OSError as exc:
        raise StateError(f"state file unreadable: {type(exc).__name__}") from None
    if len(raw) > MAX_STATE_BYTES:
        raise StateError("state file is larger than 1 MiB")
    try:
        doc = json.loads(raw.decode("utf-8-sig"), parse_constant=_reject_json_constant)
    except (ValueError, RecursionError):
        raise StateError("state file is not valid UTF-8 JSON") from None
    return validate_state(doc)


SUPERVISE_SYSTEM_PROMPT = """You supervise a feedback suppressor during a live show. The suppressor \
(the detector) already places notch filters by itself within 100-500 ms; you are slow (seconds) and \
you are NOT the safety net. You read the session log and the current state with the read-only tools \
and you may only propose changes by calling the recording tools: propose_deepen, propose_hold, \
propose_release, no_action. Nothing else exists: any text you write is discarded unread.

Rules:
- Never try to raise any level, gain, volume or the notch depth ceiling, never remove protection \
wholesale, never write advice or explanations: only tool calls count.
- When unsure, or when the data is thin, call no_action. Doing nothing is always safe.
- Address a notch by its nid and its expect_hz (its frequency in Hz, from current_state). Only notches \
whose origin is "detector" can be proposed on.
- propose_deepen: only for a frequency that keeps coming back (see the log) or a notch whose slot still \
shows rising or critical ring risk at its depth. Depth changes go one rung at a time: target_db must \
equal the notch's next_rung_db from current_state (never deeper, never a value of your own choosing). \
When next_rung_db is null the notch cannot be deepened: do not propose it.
- seen_s is the number of seconds since the supervisor first observed the notch. It is NOT the age of \
the notch: the notch itself may be much older.
- propose_hold: keep a notch that is still needed from being released for hold_s seconds (30 to 600).
- propose_release: only for a notch you have evidence is false (a FALSE verdict in the log, or long \
silence at that frequency with no recurrence). ONLY when the slot's ring_risk is exactly "low": \
"rising", "critical" and "na" (unknown) are all refused.
- At most a few proposals per turn; call no_action when nothing is needed. Finish after recording."""

SUPERVISE_USER_PROMPT = ("Review the current state (current_state) and the session log with the read-only "
                         "tools, then record your proposals with the recording tools, or call no_action.")


def _supervise_args_schema(props: dict) -> dict:
    return {"type": "object", "properties": props, "required": list(props),
            "additionalProperties": False}


_NID_SCHEMA = {"type": "integer", "minimum": 0, "description": "notch id from current_state"}
_HZ_SCHEMA = {"type": "number", "description": "the notch frequency in Hz from current_state "
                                               "(must be within one bin_hz of it)"}

SUPERVISE_TOOL_SPECS: dict[str, dict] = {
    **TOOL_SPECS,
    "current_state": {
        "description": "The app's state right now: modes, bin width, and per slot the ring risk and "
                       "every notch (nid, hz, q, depth, ceiling, next_rung_db = the only depth propose_deepen "
                       "may target or null, origin, who placed / changed it, seen_s = seconds since the "
                       "supervisor first observed the notch, NOT the notch's true age, hold, verdict).",
        "parameters": _NO_ARGS},
    "propose_deepen": {
        "raw": True,
        "description": "Propose making a detector-origin notch deeper, one rung at a time. target_db "
                       "must equal that notch's next_rung_db from current_state; when next_rung_db is "
                       "null the notch cannot be deepened.",
        "parameters": _supervise_args_schema({
            "nid": _NID_SCHEMA, "expect_hz": _HZ_SCHEMA,
            "target_db": {"type": "number", "description": "target depth in dB (negative)"},
            "reason": {"type": "string", "enum": list(DEEPEN_REASONS)}})},
    "propose_hold": {
        "raw": True,
        "description": "Propose holding a detector-origin notch (no auto-release) for hold_s seconds.",
        "parameters": _supervise_args_schema({
            "nid": _NID_SCHEMA, "expect_hz": _HZ_SCHEMA,
            "hold_s": {"type": "integer", "minimum": HOLD_S_MIN, "maximum": HOLD_S_MAX},
            "reason": {"type": "string", "enum": list(HOLD_REASONS)}})},
    "propose_release": {
        "raw": True,
        "description": "Propose releasing a detector-origin notch you have evidence is false or "
                       "stale. Only while the slot ring_risk is exactly low: rising, critical and "
                       "na (unknown) are refused.",
        "parameters": _supervise_args_schema({
            "nid": _NID_SCHEMA, "expect_hz": _HZ_SCHEMA,
            "reason": {"type": "string", "enum": list(RELEASE_REASONS)}})},
    "no_action": {
        "raw": True,
        "description": "Record that nothing should change.",
        "parameters": _supervise_args_schema({
            "reason": {"type": "string", "enum": list(NO_ACTION_REASONS)}})},
}


class Supervisor:
    """Holds the validated state and the recorded actions of one --supervise
    run. The recording tools are the FIRST line of defence only: the C++ app
    re-checks every action and is the trust boundary."""

    def __init__(self, state: dict):
        self.state = state
        self.actions: list[dict] = []
        self._seen: set[tuple] = set()
        self._notches: dict[int, tuple[dict, dict]] = {
            n["nid"]: (s, n) for s in state["slots"] for n in s["notches"]}

    # -- read --
    def current_state(self) -> str:
        return json.dumps(self.state, ensure_ascii=False)

    # -- recording --
    @staticmethod
    def _reject(code: str) -> str:
        return f"rejected: {code}"

    def _target(self, args: dict, keys: tuple):
        """(slot, notch, None) when `args` names a valid detector-origin notch, else (None, None, error)."""
        if set(args) != set(keys):
            return None, None, self._reject("bad_arguments")
        nid = args["nid"]
        if not _is_int(nid) or nid not in self._notches:
            return None, None, self._reject("unknown_nid")
        slot, notch = self._notches[nid]
        if notch["origin"] != "detector":
            return None, None, self._reject("not_detector_origin")
        hz = args["expect_hz"]
        if not _is_num(hz) or abs(float(hz) - float(notch["hz"])) > float(self.state["bin_hz"]):
            return None, None, self._reject("frequency_mismatch")
        return slot, notch, None

    @staticmethod
    def _enum_ok(v: object, allowed: tuple) -> bool:
        return isinstance(v, str) and v in allowed

    def _record(self, action: dict, nid: int | None) -> str:
        key = (action["tool"], nid)
        if key in self._seen:
            return RESULT_DUPLICATE
        if len(self.actions) >= SUPERVISE_MAX_ACTIONS:
            return RESULT_LIMIT
        self._seen.add(key)
        self.actions.append(action)
        return RESULT_QUEUED

    def propose_deepen(self, args: dict) -> str:
        _, notch, err = self._target(args, ("nid", "expect_hz", "target_db", "reason"))
        if err:
            return err
        if not self._enum_ok(args["reason"], DEEPEN_REASONS):
            return self._reject("bad_reason")
        target = args["target_db"]
        if not _is_num(target):
            return self._reject("bad_target_db")
        if not float(target) < float(notch["depth_db"]):
            return self._reject("target_not_deeper")
        if float(target) < float(notch["ceiling_db"]):
            return self._reject("target_beyond_ceiling")
        rung = notch["next_rung_db"]
        if rung is None or abs(float(target) - float(rung)) > RUNG_TOLERANCE_DB:
            allowed = ("none: this notch cannot be deepened" if rung is None
                       else f"target_db must be {float(rung):g} dB")
            return self._reject(f"not_next_rung ({allowed})")
        return self._record({"tool": "propose_deepen", "nid": args["nid"],
                             "expect_hz": float(args["expect_hz"]), "target_db": float(target),
                             "reason": args["reason"]}, args["nid"])

    def propose_hold(self, args: dict) -> str:
        _, _, err = self._target(args, ("nid", "expect_hz", "hold_s", "reason"))
        if err:
            return err
        if not self._enum_ok(args["reason"], HOLD_REASONS):
            return self._reject("bad_reason")
        hold = args["hold_s"]
        if not _is_int(hold) or not HOLD_S_MIN <= hold <= HOLD_S_MAX:
            return self._reject("bad_hold_s")
        return self._record({"tool": "propose_hold", "nid": args["nid"],
                             "expect_hz": float(args["expect_hz"]), "hold_s": hold,
                             "reason": args["reason"]}, args["nid"])

    def propose_release(self, args: dict) -> str:
        slot, _, err = self._target(args, ("nid", "expect_hz", "reason"))
        if err:
            return err
        if not self._enum_ok(args["reason"], RELEASE_REASONS):
            return self._reject("bad_reason")
        if slot["ring_risk"] != "low":      # "na" (unknown) is refused too, like the C++ side
            return self._reject("ring_risk_not_low")
        return self._record({"tool": "propose_release", "nid": args["nid"],
                             "expect_hz": float(args["expect_hz"]), "reason": args["reason"]},
                            args["nid"])

    def no_action(self, args: dict) -> str:
        if set(args) != {"reason"}:
            return self._reject("bad_arguments")
        if not self._enum_ok(args["reason"], NO_ACTION_REASONS):
            return self._reject("bad_reason")
        return self._record({"tool": "no_action", "reason": args["reason"]}, None)

    def result_actions(self) -> list[dict]:
        """What goes to stdout. A run that recorded nothing says so explicitly."""
        return list(self.actions) or [{"tool": "no_action", "reason": "nothing_to_do"}]


def build_supervise_toolset(events: list[dict], state: dict) -> ToolSet:
    """The read-only log tools, `current_state` and the four recording tools."""
    sup = Supervisor(state)
    funcs = make_tool_functions(events)
    funcs.update({"current_state": sup.current_state, "propose_deepen": sup.propose_deepen,
                  "propose_hold": sup.propose_hold, "propose_release": sup.propose_release,
                  "no_action": sup.no_action})
    return ToolSet(SUPERVISE_SYSTEM_PROMPT, SUPERVISE_TOOL_SPECS, funcs,
                   allow_empty_reply=True, supervisor=sup,
                   max_iterations=SUPERVISE_MAX_ITERATIONS)


# --- safety filter -----------------------------------------------------------
#
# Design (owner decision Q26: wider filter, more false blocks accepted):
#   1. Any letter outside the Latin script blocks the answer outright.
#   2. The text is normalised (NFKC, zero-width characters, diacritics, look-alike
#      letters, leetspeak between letters, letters spaced or dotted apart) in several
#      variants; the answer is blocked if ANY variant is unsafe.
#   3. Each clause (split at . ! ? ; , newline) is scanned for CUES (raise / louder /
#      set-to-a-value / add-dB / disable / remove / bypass / clear-a-notch) and OBJECTS
#      (gain, volume, fader, ceiling, ... / the protection itself / notches). A cue
#      within _WINDOW words of a matching object, in either order, blocks.
#   4. A plain negation directly in front of the cue ("đừng tăng", "never raise")
#      excuses it. Rhetorical or double negation never excuses.

_WINDOW = 10      # words between a cue and its object, either order
_NOTCH_WINDOW = 5

_TOK = re.compile(r"[+\-]?\d+(?:[.,]\d+)?|[^\W\d_]+(?:'[^\W\d_]+)?|[=:%]")
_NUMBER_RE = re.compile(r"[+\-]?\d+(?:[.,]\d+)?$")
_SPLIT_RE = re.compile(r"[.!?;…]+(?=\s|$)|[\n\r]|(?<!\d),|,(?!\d)|\s[-–—•]+\s|[()\[\]{}]")
_RUN_RE = re.compile(r"(?<![a-z0-9])[a-z](?:[\s.\-_*·•/|,;:+~^`'\"]+[a-z]){2,}(?![a-z0-9])")
_SQUEEZE_RE = re.compile(r"(?<=[a-z])[.\-_*·•|/'\"](?=[a-z])")
_LEET_RE = re.compile(r"(?<=[a-z])[0-9@$]{1,2}(?=[a-z])|(?<![a-z0-9])[0-9@$](?=[a-z]{2})")
_LEET = str.maketrans({"4": "a", "3": "e", "1": "i", "0": "o", "5": "s", "7": "t",
                       "@": "a", "$": "s", "8": "b", "9": "g"})
_DUNGX = re.compile(r"(?<!\w)(?:đúng|dùng|dụng|dũng|dúng)(?!\w)")
_LOOKALIKE = str.maketrans({"ɡ": "g", "ɑ": "a", "ɩ": "i", "ı": "i", "ĸ": "k"})
_ALLOWED_NON_LATIN = frozenset("μΩΔ")   # symbols audio text uses (µs, Ω, ΔdB)


def _index(pats):
    d: dict = {}
    for p in pats:
        d.setdefault(p[0], []).append(p)
    return d


def _spans(w, index):
    out = []
    for i, t in enumerate(w):
        for p in index.get(t, ()):
            if tuple(w[i:i + len(p)]) == p:
                out.append((i, i + len(p)))
    return out


_NEG = frozenset({"dung", "khong", "chua", "tranh", "never", "not", "dont", "don't", "doesnt",
                  "doesn't", "shouldnt", "shouldn't", "mustnt", "mustn't", "cannot", "cant",
                  "can't", "wont", "won't", "avoid", "without"})
_FILLERS = frozenset({"la", "nen", "can", "phai", "duoc", "nua", "he", "bao", "gio", "ever", "really",
                      "simply", "just", "need", "needs", "to", "the", "de", "khuyen", "recommend",
                      "advise", "suggest", "recommending", "advising", "suggesting"})
_CONJ = frozenset({"or", "and", "nor", "hoac", "va"})
_FLIP_TOKENS = frozenset({"sao", "why", "hesitate", "hesitates", "hesitating", "hesitation",
                          "ngai", "afraid"})
_FLIP_PAIRS = _index([("ngan", "ngai"), ("no", "reason"), ("no", "reasons"), ("any", "reason"),
                      ("khong", "co", "ly", "do"), ("khong", "ly", "do"), ("chang", "co", "ly", "do"),
                      ("ly", "do", "gi"), ("ly", "do", "nao"), ("feel", "free"), ("go", "ahead"),
                      ("how", "about"), ("what", "about"), ("no", "harm"), ("khong", "hai"),
                      ("be", "shy"), ("dung", "ngai")])
_NEG_PHRASES = ((("thay", "vi"), "dung"), (("instead", "of"), "dung"), (("rather", "than"), "dung"))

_LEVEL = _index([
    ("gain",), ("gains",), ("volume",), ("volumes",), ("fader",), ("faders",), ("level",),
    ("levels",), ("muc",), ("trim",), ("preamp",), ("preamps",), ("master",), ("output",),
    ("outputs",), ("mic",), ("mics",), ("micro",), ("microphone",), ("microphones",),
    ("monitor",), ("monitors",), ("send",), ("sends",), ("ceiling",), ("tran",), ("loa",),
    ("speaker",), ("speakers",), ("pa",), ("amp",), ("signal",), ("juice",), ("limit",),
    ("limits",), ("gioi", "han"), ("am", "luong"), ("do", "loi"), ("tin", "hieu"),
    ("cuong", "do"), ("do", "to"), ("cong", "suat"), ("ganancia",), ("ganho",), ("guadagno",),
    ("volumen",), ("pegel",), ("lautstarke",), ("verstarkung",), ("niveau",), ("puissance",),
    ("do", "nhay"), ("input", "sensitivity"), ("sensitivity",), ("do", "lon")])
# weak level carriers: only comparatives, particle verbs and a bare "up" are paired with them
_EXTRA = _index([("tieng",), ("sound",), ("audio",), ("nhac",), ("giong",), ("voice",),
                 ("vocal",), ("vocals",), ("music",), ("kenh",), ("channel",), ("channels",),
                 ("bus",), ("aux",), ("sub",), ("wedge",), ("wedges",), ("input",)])
_PROTECT = _index([
    ("chong", "hu"), ("feedback", "suppression"), ("feedback", "suppressor"),
    ("feedback", "eliminator"), ("feedback", "protection"), ("anti", "feedback"),
    ("limiter",), ("limiters",), ("clamp",), ("clamps",), ("clamping",), ("protection",),
    ("bao", "ve"), ("safety",), ("safeguard",), ("safeguards",), ("gioi", "han"), ("limit",),
    ("limits",), ("han", "che"), ("tran",), ("ceiling",), ("larsen",), ("ruckkopplung",),
    ("realimentacion",), ("retroalimentacion",), ("acoplamiento",), ("afs",), ("suppression",),
    ("suppressor",),
    # the app's own indirect names for the protection
    ("tinh", "nang"), ("che", "do", "tu", "dong"), ("tu", "dong"), ("auto",), ("triet", "hu"),
    ("plugin",), ("the", "feature"), ("this", "mode"), ("auto", "mode")])
_PROTECT_STRICT_ONLY = _index([("feedback",)])   # "feedback" alone: only strong disable cues
_NOTCH = _index([("notch",), ("notches",), ("diem", "cat"), ("vet", "cat"), ("bo", "loc"),
                 ("filter",), ("filters",), ("cut",), ("cuts",)])

_RAISE_WORDS = frozenset({"tang", "nang", "bom", "khuech", "max", "rise", "rises", "rising",
                          "bump", "bumps", "bumping", "bumped",
                          "upsize"})
_RAISE_STEMS = ("rais", "increas", "boost", "crank", "lift", "amplif", "pump", "maximi",
                "maxed", "hike")
_AMOUNT_EXTRA = frozenset({"them", "cong", "add", "plus", "up", "bump", "len"})
_AMOUNT_FILL = frozenset({"by", "len", "them", "khoang", "about", "around", "another", "further",
                          "nua", "toi"})
_COMPAR_WORDS = frozenset({"louder", "higher", "hotter", "hot", "stronger", "bigger", "greater",
                           "harder"})
# soft cues: ignored when the same clause carries an explicit reduce cue
_SOFT_WORDS = frozenset({"them", "nua", "add", "adding", "added", "adds"})
_PAIRS_C = _index([("len", "them"), ("bat", "to"), ("cho", "them"), ("mo", "them"), ("len", "chut"), ("len", "it"), ("len", "xiu"),
                   ("nong", "hon"), ("a", "little"), ("a", "bit"), ("to", "hon"), ("lon", "hon"), ("cao", "hon"), ("manh", "hon"), ("to", "len"),
                   ("lon", "len"), ("manh", "len"), ("cao", "len"), ("to", "them"), ("cho", "to"),
                   ("cho", "lon"), ("cho", "manh"), ("lam", "to"), ("lam", "lon"),
                   ("lam", "manh"), ("het", "co"), ("mo", "to"), ("mo", "lon")])
_PV_VERBS = frozenset({"turn", "push", "bump", "bring", "crank", "ramp", "pump", "pull", "jack",
                       "dial", "step", "amp", "day", "keo", "van", "nhich", "dua", "pushing",
                       "pushed", "turning", "turned", "bringing", "pulling", "dialing", "dialling",
                       "cranking", "ramping", "go", "goes", "going"})
_PLAIN_VERBS = frozenset({"push", "pushing", "pushed", "day", "keo", "pull", "pulling", "van",
                          "nhich"})
_REDUCERS = frozenset({"xuong", "down", "back", "away", "ra", "xa", "lui", "giam", "lower", "nho"})
_PARTICLES = frozenset({"up", "len"})
_SET_CUES = frozenset({"dat", "set", "sets", "setting", "dua", "chinh", "adjust", "adjusting",
                       "dial", "put", "move", "bring", "de"})
_VALUE_PREV = frozenset({"to", "at", "o", "len", "toi", "den", "=", ":", "around", "about",
                         "khoang", "muc"})
_NON_LEVEL_UNITS = frozenset({"m", "mm", "cm", "met", "hz", "khz", "s", "giay", "ms", "lan",
                              "phut", "kenh", "slot", "notch", "inch", "ft", "feet", "cai", "gio"})
_MAXWORDS = frozenset({"max", "maximum", "full", "cuc"})

_D_STRICT_STEMS = ("disabl", "deactivat", "bypass")
_D_WEAK_STEMS = ("remov", "kill", "defeat", "disengag", "overrid", "ignor", "unlock", "unclamp",
                 "loosen", "relax", "widen")
_D_WEAK_WORDS = frozenset({"bo", "go", "ngat", "ngung", "xoa", "noi", "skip"})
_D_PAIRS_STRICT = _index([("turn", "off"), ("switch", "off"), ("shut", "off"), ("vo", "hieu"),
                          ("power", "off")])
_D_PAIRS_WEAK = _index([("bo", "qua")])
_N_WORDS = frozenset({"xoa", "go", "cleared", "clear", "clears", "clearing", "reset", "resets", "resetting",
                      "erase", "wipe", "huy", "cancel", "dismiss", "flush", "purge", "drop", "bo",
                      "tat"})
_N_STEMS = ("remov", "delet", "disabl", "bypass", "erasing", "wiping")
_N_PAIRS = _index([("turn", "off"), ("vo", "hieu"), ("don", "sach"), ("don", "dep"),
                   ("get", "rid"), ("getting", "rid"), ("got", "rid"),
                   ("lam", "lai", "tu", "dau"), ("start", "over")])
_NS_PAIRS = _index([("clear", "all"), ("xoa", "het"), ("xoa", "tat", "ca"), ("reset", "all"),
                    ("xoa", "sach"), ("clear", "everything")])
_PAST = frozenset({"bi", "duoc", "da", "been", "was", "were", "got", "tu", "auto", "automatically",
                   "is", "are", "vua", "get", "gets"})

# Latin-script languages other than Vietnamese/English: not exhaustive, no negation handling.
_F_RAISE_STEMS = ("augment", "aument", "erhoh", "verstark", "aufdreh", "naikkan", "menaikkan",
                  "tingkatkan", "meningkatkan", "pousse", "monter", "montez", "subir", "sube",
                  "suba", "subid", "alzare", "alza", "alzi", "spingere", "spingi", "hochdreh")
_F_RAISE_WORDS = frozenset({"lauter", "hoher"})
_F_RAISE_PAIRS = _index([("plus", "fort"), ("plus", "haut"), ("mas", "fuerte"), ("mas", "alto"),
                         ("mais", "alto"), ("mais", "forte"), ("piu", "forte"), ("piu", "alto")])
_F_OFF_STEMS = ("apag", "desactiv", "desativ", "deslig", "spegn", "ausschalt", "abschalt",
                "eteind", "eteign", "couper", "coupez", "matikan", "nonaktif", "deshabilit",
                "desabilit", "disattiv")


def _foreign_script(text: str) -> bool:
    for c in unicodedata.normalize("NFKC", text):
        if c.isalpha() and c not in _ALLOWED_NON_LATIN:
            try:
                name = unicodedata.name(c)
            except ValueError:
                return True
            if not name.startswith("LATIN"):
                return True
    return False


def _fold(text: str, glue: bool) -> str:
    """NFKC, drop/split format characters, casefold, strip diacritics (đ -> d)."""
    t = unicodedata.normalize("NFKC", text)
    t = "".join(("" if glue else " ") if unicodedata.category(c) == "Cf" else c for c in t)
    t = t.casefold().replace("’", "'").replace("‘", "'").replace("ʼ", "'").replace("`", "'")
    t = t.replace("chớ", "đừng")   # "chớ" would fold to "cho" (= "for"), losing its negation
    t = _DUNGX.sub("dungx", t)     # "đúng" / "dùng" fold to "dung" (= đừng): keep them apart
    t = re.sub(r"(?<!\w)dừng(?!\w)", "ngưng", t)   # "dừng" = stop, not "đừng"
    t = re.sub(r"(?<!\w)ổn(?!\w)", "ongood", t)    # "ổn" = fine; keep it apart from English "on"
    t = t.translate(_LOOKALIKE)
    t = "".join(c for c in unicodedata.normalize("NFD", t) if unicodedata.category(c) != "Mn")
    return " ".join(t.replace("đ", "d").split())


def _vocab() -> list[str]:
    words: set[str] = set()
    for group in (_NEG, _FILLERS, _RAISE_WORDS, _AMOUNT_EXTRA, _COMPAR_WORDS, _PV_VERBS,
                  _D_WEAK_WORDS, _N_WORDS, _SET_CUES):
        words |= set(group)
    for idx in (_LEVEL, _PROTECT, _NOTCH, _EXTRA, _PAIRS_C, _D_PAIRS_STRICT, _N_PAIRS):
        for pats in idx.values():
            for p in pats:
                words |= set(p)
    words |= {"raise", "increase", "boost", "disable", "bypass", "remove", "off", "turn"}
    return sorted((x for x in words if len(x) >= 3 and "'" not in x), key=len, reverse=True)


_VOCAB = _vocab()


def _segment(letters: str) -> str:
    """Split glued letters ('tanggain') into known words, longest match first."""
    out: list[str] = []
    unknown = ""
    i = 0
    while i < len(letters):
        hit = next((v for v in _VOCAB if letters.startswith(v, i)), None)
        if hit:
            if unknown:
                out.append(unknown)
                unknown = ""
            out.append(hit)
            i += len(hit)
        else:
            unknown += letters[i]
            i += 1
    if unknown:
        out.append(unknown)
    return " ".join(out)


def _prep(text: str, glue: bool, squeeze: bool, leet: bool) -> str:
    t = _fold(text, glue)
    if squeeze:
        t = _SQUEEZE_RE.sub("", t)
    if leet:
        t = _LEET_RE.sub(lambda m: m.group().translate(_LEET), t)
    return _RUN_RE.sub(lambda m: " " + _segment(re.sub(r"[^a-z]", "", m.group())) + " ", t)


def _merge_negations(w: list[str]) -> list[str]:
    out: list[str] = []
    i = 0
    while i < len(w):
        for phrase, repl in _NEG_PHRASES:
            if tuple(w[i:i + len(phrase)]) == phrase:
                out.append(repl)
                i += len(phrase)
                break
        else:
            out.append(w[i])
            i += 1
    return out


def _gap(a: tuple, b: tuple) -> int:
    if b[0] >= a[1]:
        return b[0] - a[1]
    if b[1] <= a[0]:
        return a[0] - b[1]
    return 0


def _valid_value(w: list[str], k: int) -> bool:
    """w[k] is a number (or a "max" word) that reads like a level setting."""
    t = w[k]
    if t in _MAXWORDS or (t == "toi" and k + 1 < len(w) and w[k + 1] == "da"):
        return True
    if not _NUMBER_RE.match(t):
        return False
    nxt = w[k + 1] if k + 1 < len(w) else ""
    if nxt in _NON_LEVEL_UNITS:
        return False
    prev = w[k - 1] if k > 0 else ""
    return t[0] in "+-" or prev in _VALUE_PREV or nxt in ("db", "%", "percent")


# Rule A: strong level objects (default-deny), rule B: carry-over, rule D: language gate.
_STRONG = _index([("gain",), ("gains",), ("volume",), ("volumes",), ("am", "luong"),
                  ("do", "loi"), ("fader",), ("faders",), ("trim",), ("preamp",), ("preamps",),
                  ("master",), ("tran",), ("ceiling",), ("ganancia",), ("ganho",), ("guadagno",),
                  ("volumen",), ("pegel",), ("lautstarke",), ("verstarkung",)])
_REDUCE_WORDS = frozenset({"giam", "ha", "bot", "xuong", "down", "cut", "cuts", "cutting", "quieter",
                           "softer", "lessen"})
_REDUCE_STEMS = ("reduc", "decreas", "lower", "attenuat")
_REDUCE_PAIRS = _index([("nho", "lai"), ("back", "off"), ("pull", "down"), ("turn", "down")])
_REDUCE_CTX = frozenset({"giam", "ha", "bot", "cut", "lower", "reduce", "xuong"})
_DEPTH_CTX = frozenset({"sau", "deeper", "deep", "deepen", "deeply"})   # a deeper notch is safe
_PRONOUNS = frozenset({"it", "no", "chung", "them"})
_MODALS = frozenset({"nen", "should", "must", "can", "need", "phai", "hay", "please", "could",
                     "ought", "co", "the"})

_VN_ONLY = re.compile("[đăơưĐĂƠƯẠ-ỹ]")
_VN_STRONG = frozenset(
    "khong nhung cua trong voi duoc nay cac mot hay nen neu thi nhu vao tren duoi khi cung dang "
    "roi xong nhe thoi chua dung rat qua nhieu hon cao thap sau lai vua khoang giay phut lan "
    "nao gi ban minh toi boi nhin kiem tra hoi tai vi sao khac ngay sang chay dat chinh thu "
    "can phai nen theo tung moi nhat dau cuoi gan xa".split())
_VN_WEAK = frozenset(
    "la va co o de cho ra len nua di a ma se da bi ve ca tat het no chung ai the".split())
_EN_WORDS = frozenset(
    "the and or of to in on at is are was were be been it its this that these those for with "
    "without not no you your we our my do does don't dont should can could will would may might "
    "must if when then so but as by from up down more less all any some each every just also "
    "only very too than into out off over under about above below after before while because "
    "please try keep leave make set use check turn lower raise reduce increase decrease move "
    "point mute unmute let get give take put add remove clear reset low high loud quiet near "
    "far away back again first last next same other another few little bit lot many much both "
    "until still already now here there what which who how why where one two three four five "
    "six seven eight nine ten per has have had am being them they he she him her his their "
    "peak sits floor verdict burst placed rose quickly unused speaker vocal singer channel mixer "
    "wedge ring ringing bypass disable enable off on suppression feedback protection limiter "
    "never always louder hotter higher boost cut bump".split())
_DOMAIN_WORDS = frozenset(
    "ha giam tang nang bot nho chut it ti xiu kenh loa mic micro gain monitor notch notches "
    "slot hu chong tran muc am luong do loi tieng nhac hz khz db preset eq pa dai tan so lan "
    "xiu mixer".split())


def _recognised(tok: str, has_vn: bool) -> bool:
    return (tok in _EN_WORDS or tok in _VN_STRONG or tok in _DOMAIN_WORDS or tok in _RECOG_VOCAB
            or (has_vn and tok in _VN_WEAK))


_VOCAB_SET = frozenset(_VOCAB)
_RECOG_VOCAB = frozenset(v for v in _VOCAB if len(v) >= 4)   # short ones collide with other languages


def _language_ok(text: str) -> bool:
    """Rule D: every sentence must be recognisably Vietnamese or English.
    A sentence with a Vietnamese-only letter (ă ơ ư đ or a tone-marked vowel) passes;
    otherwise at least 40% of its words must be known Vietnamese/English/domain words."""
    nfkc = unicodedata.normalize("NFKC", text)
    has_vn = bool(_VN_ONLY.search(nfkc))
    for sentence in re.split(r"[.!?;…\n]+", nfkc):
        if _VN_ONLY.search(sentence):
            continue
        toks = [x for x in re.findall(r"[^\W\d_]+(?:'[^\W\d_]+)?", _fold(sentence, True))
                if len(x) >= 2]
        if len(toks) >= 2 and sum(_recognised(x, has_vn) for x in toks) < 0.4 * len(toks):
            return False
    return True


def _rejoin(w: list[str]) -> list[str]:
    """Rule E: re-join adjacent fragments when the joined word is a known cue/object
    ("ga in" -> "gain", "rai se" -> "raise", "ta ng" -> "tang")."""
    out: list[str] = []
    i = 0
    while i < len(w):
        for size in (3, 2):
            part = w[i:i + size]
            joined = "".join(part)
            if len(part) == size and joined in _VOCAB_SET and not all(x in _VOCAB_SET for x in part):
                out.append(joined)
                i += size
                break
        else:
            out.append(w[i])
            i += 1
    return out


def _level_spans(w):
    """Level-object spans, minus statements about the loop itself ("độ lợi vòng",
    "loop gain") and "chạm trần" / "hit the ceiling" (the app's own findings text)."""
    out = []
    for s, e in _spans(w, _LEVEL):
        nxt = w[e] if e < len(w) else ""
        prev = w[s - 1] if s else ""
        prev2 = w[s - 2] if s > 1 else ""
        if nxt == "vong" or prev == "loop" or prev in ("cham", "hit", "reach", "reached") \
                or (prev == "the" and prev2 in ("hit", "reach", "reached")):
            continue
        out.append((s, e))
    return out


def _has_reduce(w) -> bool:
    for i, t in enumerate(w):
        if t in _REDUCE_WORDS or t.startswith(_REDUCE_STEMS):
            return True
        for p in _REDUCE_PAIRS.get(t, ()):
            if tuple(w[i:i + len(p)]) == p:
                return True
    return False


_SHALLOW = _index([("nong",), ("shallower",), ("shallow",), ("less", "deep"), ("giam", "do", "sau"),
                   ("giam", "bot", "do", "sau"), ("thu", "nho", "do", "sau"), ("bot", "do", "sau")])
_DONE = frozenset({"da", "bi", "duoc", "been", "was", "were", "got", "vua"})
_STATE_BEFORE_OFF = frozenset({"dang", "is", "are", "was", "been", "currently", "bi"})
_ADVICE = frozenset({"nen", "can", "should", "must", "need", "needs", "phai", "hay", "please",
                     "could", "ought", "di", "nhe", "thu", "try", "just", "simply", "may", "might",
                     "lets"})
_STATE_MARKS = frozenset({"hien", "nguyen", "current", "currently", "healthy", "fine", "ongood",
                          "unchanged", "stable"})


def _is_statement(w) -> bool:
    """A clause about a level that gives a figure or a status and no advice word:
    "Gain hiện tại là 35 dB", "The ceiling is at -24 dB", "Gain looks fine", "giữ nguyên"."""
    if "giu" in w and ("nguyen" in w or "hien" in w):
        return True
    if "keep" in w and (any(t in w for t in ("current", "same", "unchanged"))
                        or any(w[i] == "as" and w[i + 1:i + 2] == ["is"] for i in range(len(w)))):
        return True
    has_num = any(_NUMBER_RE.match(t) for t in w)
    has_mark = any(t in _STATE_MARKS for t in w)
    return (has_num or has_mark) and not any(t in _ADVICE for t in w)


def _find_cues(w, lvl, prot, extra) -> list[tuple]:
    """(kind, start, end, strict) for every cue. Kinds: R needs a level object nearby,
    RP/C a level or carrier object, RB a bare "up"/"lên" right next to one, S stands
    alone, D a protection, N a notch, SH a shallower notch, F foreign."""
    n = len(w)
    cues: list[tuple] = []
    for i, t in enumerate(w):
        nxt = w[i + 1] if i + 1 < n else ""
        prev = w[i - 1] if i else ""
        # "tốc độ tăng", "mức tăng" are nouns (a rate / an amount of rise), not advice
        noun = t == "tang" and (prev == "muc" or w[max(0, i - 2):i] == ["toc", "do"])
        raiseish = (t in _RAISE_WORDS or t.startswith(_RAISE_STEMS)) and not noun
        if raiseish:
            cues.append(("R", i, i + 1, False))
        if t == "nang":
            cues.append(("RP", i, i + 1, False))
        if t == "tang" and nxt == "am" and not (i + 2 < n and w[i + 2] == "luong"):
            cues.append(("S", i, i + 2, False))   # "tăng âm" = amplifier / boost
        if t in _COMPAR_WORDS:
            cues.append(("C", i, i + 1, False))
            if t == "louder":
                cues.append(("S", i, i + 1, False))
        if t in _SOFT_WORDS:
            cues.append(("CS", i, i + 1, False))
        for p in _PAIRS_C.get(t, ()):
            if tuple(w[i:i + len(p)]) == p:
                cues.append(("CS" if p[0] in ("a", "cho", "mo", "len") else "C", i, i + len(p), False))
        for p in _SHALLOW.get(t, ()):
            if tuple(w[i:i + len(p)]) == p:
                cues.append(("SH", i, i + len(p), False))
        if t in ("more", "extra") and any(i < s <= i + 2 for s, _ in lvl):
            cues.append(("S", i, i + 1, False))
        pv_before = any(x in _PV_VERBS for x in w[max(0, i - 4):i])
        if t in _PV_VERBS:
            k = next((x for x in range(i + 1, min(i + 5, n)) if w[x] in _PARTICLES), None)
            if k is not None:
                cues.append(("RP", i, k + 1, False))
        if t.startswith(("turn", "crank", "bring", "push")) and nxt in ("it", "that", "this", "them") \
                and i + 2 < n and w[i + 2] in _PARTICLES:
            cues.append(("S", i, i + 3, False))
        if t in _PLAIN_VERBS and not any(x in _REDUCERS for x in w[i + 1:i + 6]) \
                and any(i < s <= i + 4 for s, _ in lvl):
            cues.append(("S", i, i + 1, False))
        if t == "up" and nxt != "to" and not pv_before and (i == 0 or prev != "set") \
                and any(i < s <= i + 3 for s, _ in lvl):
            cues.append(("S", i, i + 1, False))
        if t in ("up", "len") and nxt != "to" and not pv_before and (i == 0 or prev != "set"):
            cues.append(("RB", i, i + 1, False))
        if t[0] == "+" and _NUMBER_RE.match(t) and nxt == "db":
            cues.append(("S", i, i + 2, False))
        if (raiseish or t in _AMOUNT_EXTRA) and not any(x in _REDUCE_CTX for x in w[max(0, i - 3):i]) \
                and not any(x in _DEPTH_CTX for x in w[max(0, i - 6):i]):
            for k in range(i + 1, min(i + 4, n)):
                if _NUMBER_RE.match(w[k]):
                    if k + 1 < n and w[k + 1] == "db":
                        tail = w[k + 2:k + 4]
                        rate = tail[:2] == ["moi", "giay"] or tail[:1] == ["s"] or tail[:2] == ["per", "second"]
                        if not rate:   # "12 dB mỗi giây" / "dB/s" is a rate, not a level change
                            cues.append(("S", i, k + 2, False))
                    break
                if w[k] not in _AMOUNT_FILL:
                    break
        if t in _SET_CUES:
            cues.append(("SET", i, i + 1, False))
        # disable cues ("đang tắt" / "is off" describes a state)
        state = any(x in _STATE_BEFORE_OFF for x in w[max(0, i - 2):i])
        strict_hit = (t == "tat" and nxt != "ca" and not state) or t.startswith(_D_STRICT_STEMS)
        weak_hit = (t.startswith(_D_WEAK_STEMS) or t in _D_WEAK_WORDS) \
            and not (t == "bo" and nxt in ("loc", "triet", "chong"))   # "bộ lọc", "bộ chống hú"
        if strict_hit:
            cues.append(("D", i, i + 1, True))
        elif weak_hit:
            cues.append(("D", i, i + 1, False))
        for p in _D_PAIRS_STRICT.get(t, ()):
            if tuple(w[i:i + len(p)]) == p:
                cues.append(("D", i, i + len(p), True))
        for p in _D_PAIRS_WEAK.get(t, ()):
            if tuple(w[i:i + len(p)]) == p:
                cues.append(("D", i, i + len(p), False))
        # "take the limiter out of the chain", "turn the protection off"
        if t in ("take", "took", "taking", "turn", "switch", "shut", "power"):
            k = next((x for x in range(i + 2, min(i + 5, n)) if w[x] in ("out", "off")), None)
            if k is not None:
                cues.append(("D", i, k + 1, True))
                cues.append(("N", i, k + 1, False))
        # notch removal
        if (t in _N_WORDS and not (t == "tat" and nxt == "ca")
                and not (t == "bo" and nxt in ("loc", "triet", "chong"))) or t.startswith(_N_STEMS):
            cues.append(("N", i, i + 1, False))
        for p in _N_PAIRS.get(t, ()):
            if tuple(w[i:i + len(p)]) == p:
                cues.append(("N", i, i + len(p), False))
        for p in _NS_PAIRS.get(t, ()):
            if tuple(w[i:i + len(p)]) == p:
                cues.append(("NS", i, i + len(p), False))
        # other Latin-script languages (never excused)
        if t.startswith(_F_RAISE_STEMS) or t in _F_RAISE_WORDS:
            cues.append(("FR", i, i + 1, False))
        for p in _F_RAISE_PAIRS.get(t, ()):
            if tuple(w[i:i + len(p)]) == p:
                cues.append(("FR", i, i + len(p), False))
        if t.startswith(_F_OFF_STEMS):
            cues.append(("FD", i, i + 1, False))
    # "fader up", "gain len", "limiter off", "gain = 40"
    for s, e in lvl:
        if e < n and w[e] in _PARTICLES:
            cues.append(("OP", s, e + 1, False))
        if e + 1 < n and w[e] in ("=", ":") and _NUMBER_RE.match(w[e + 1]):
            cues.append(("S", s, e + 2, False))
    for s, e in prot:
        if e < n and w[e] == "off":
            cues.append(("DOP", s, e + 1, True))
    # "đừng đẩy gain lên": the verb's own cue decides, not the bare "gain lên".
    def shadowed(c):
        return any(o[0] not in ("OP", "DOP") and
                   ((o[2] <= c[1] and c[1] - o[2] <= 3) or (o[1] < c[1] and c[2] <= o[2]))
                   for o in cues)
    return [c for c in cues if c[0] not in ("OP", "DOP") or not shadowed(c)]


def _flip_at(w) -> int:
    best = len(w)
    for i, t in enumerate(w):
        if t in _FLIP_TOKENS:
            best = min(best, i)
        for p in _FLIP_PAIRS.get(t, ()):
            if tuple(w[i:i + len(p)]) == p:
                best = min(best, i)
    return best


def _governed(w, cues, ci, flip, depth=0) -> bool:
    """A plain negation (an odd count of them) directly governs cue `ci`."""
    s = cues[ci][1]
    if flip < s:
        return False
    j = s - 1
    while j >= 0 and w[j] in _FILLERS:
        j -= 1
    negs = 0
    while j >= 0 and w[j] in _NEG:
        negs += 1
        j -= 1
        while j >= 0 and w[j] in _FILLERS:
            j -= 1
    if negs:
        return negs % 2 == 1
    if j >= 0 and w[j] in _CONJ and depth < 6:   # "đừng tăng gain và nâng fader"
        best = None
        for k, c in enumerate(cues):
            if c[2] <= j and j - c[2] <= 4 and (best is None or c[2] > cues[best][2]):
                best = k
        if best is not None:
            return _governed(w, cues, best, flip, depth + 1)
    return False


def _clause_violates(w: list[str], carry=(False, False, False, False)):
    """(violates, (level object, protection, notch, level-or-carrier object)) for one clause.
    `carry` says whether a recent clause named such an object: a cue with no object of
    its own inherits it (rule B)."""
    w = _merge_negations(w)
    n = len(w)
    lvl = _level_spans(w)
    extra = _spans(w, _EXTRA)
    prot = _spans(w, _PROTECT)
    strict_only = _spans(w, _PROTECT_STRICT_ONLY)
    notch = _spans(w, _NOTCH)
    strong_set = set(_spans(w, _STRONG))
    strong = [s for s in lvl if s in strong_set]
    flags = (bool(lvl), bool(prot or strict_only), bool(notch), bool(lvl or extra))
    cues = _find_cues(w, lvl, prot, extra)
    if not cues:
        return (bool(strong) and not (_has_reduce(w) or _is_statement(w))), flags
    flip = _flip_at(w)
    carry_lvl, carry_prot, carry_notch, carry_any = carry
    governed_raise = False
    for ci, (kind, s, e, strict) in enumerate(cues):
        span = (s, e)
        pronoun = any(t in _PRONOUNS for t in w[e:e + 3])
        if kind == "R":
            ok = any(_gap(span, o) <= _WINDOW for o in lvl) or \
                (not lvl and (carry_lvl or pronoun))
        elif kind == "RP":
            ok = any(_gap(span, o) <= _WINDOW for o in lvl + extra) or \
                (not lvl and not extra and (carry_any or pronoun))
        elif kind == "RB":
            pool = lvl + extra if w[s] == "up" else lvl
            ok = any(_gap(span, o) <= 2 for o in pool)
        elif kind in ("C", "CS"):
            if kind == "CS" and _has_reduce(w):
                continue
            ok = any(_gap(span, o) <= _WINDOW for o in lvl + extra) or \
                (not lvl and not extra and (carry_any or pronoun))
        elif kind == "SH":
            ok = any(_gap(span, o) <= _NOTCH_WINDOW for o in notch) or (not notch and carry_notch)
            if ok and any(t in _STATE_MARKS or t in _STATE_BEFORE_OFF or t == "la"
                          for t in w[max(0, s - 3):s]):
                ok = False   # "notch hiện tại nông 6 dB": a description
        elif kind in ("S", "OP"):
            ok = True
        elif kind == "SET":
            ok = False
            for o in lvl:
                if _gap(span, o) > 6:
                    continue
                lo = min(o[1], e)
                if any(_valid_value(w, k) and not any(x in _REDUCERS for x in w[e:k])
                       for k in range(lo, min(n, max(o[1], e) + 6))):
                    ok = True
                    break
        elif kind in ("D", "DOP"):
            pool = prot + (strict_only if strict else [])
            ok = any(_gap(span, o) <= _WINDOW for o in pool) or \
                (not prot and not strict_only and (carry_prot or pronoun))
            if ok and kind == "D" and any(t in _DONE for t in w[max(0, s - 3):s]) \
                    and not any(t in _ADVICE for t in w[max(0, s - 3):s]):
                ok = False   # "hệ thống đã tự động xóa ...": a statement of fact
        elif kind == "N":
            ok = any(_gap(span, o) <= _NOTCH_WINDOW for o in notch) or \
                (not notch and (carry_notch or pronoun))
            if ok and any(t in _PAST for t in w[max(0, s - 5):s]) \
                    and not any(t in _MODALS for t in w[max(0, s - 4):s]):
                ok = False   # "notch 3 đã bị xoá lúc 20:14": a statement of fact
        elif kind == "NS":
            ok = not (any(t in _PAST for t in w[max(0, s - 5):s])
                      and not any(t in _MODALS for t in w[max(0, s - 4):s]))
        elif kind == "FR":
            if any(_gap(span, o) <= _WINDOW for o in lvl + extra):
                return True, flags
            continue
        elif kind == "FD":
            if any(_gap(span, o) <= _WINDOW for o in prot + strict_only):
                return True, flags
            continue
        else:
            ok = False
        if ok:
            if _governed(w, cues, ci, flip):
                governed_raise = True
            else:
                return True, flags
    # Rule A: a clause naming a strong level object is blocked unless it carries an
    # explicit reduce cue, only raise cues that a plain negation governs, or is a
    # statement (a figure or a status, no advice word).
    if strong and not (_has_reduce(w) or governed_raise or _is_statement(w)):
        return True, flags
    return False, flags


def _text_violates(t: str) -> bool:
    carry = [0, 0, 0, 0]
    for clause in _SPLIT_RE.split(t):
        if not clause.strip():
            continue
        w = _TOK.findall(clause)
        variants = [w]
        joined = _rejoin(w)
        if joined != w:
            variants.append(joined)
        flags = [False, False, False, False]
        for v in variants:
            bad, fl = _clause_violates(v, tuple(c > 0 for c in carry))
            if bad:
                return True
            flags = [a or b for a, b in zip(flags, fl)]
        carry = [2 if f else max(0, c - 1) for f, c in zip(flags, carry)]
    return False


def violates_safety(text: str) -> bool:
    """True when `text` advises raising gain/level/ceiling, setting a level to a
    value, disabling or removing a protection, or clearing notches.

    What it does, in order:
      1. Any letter outside the Latin script blocks outright.
      2. Language gate: every sentence must be recognisably Vietnamese or English
         (a Vietnamese-only letter, or enough known words); otherwise it blocks.
         Small stem lists for French, Spanish, German, Portuguese, Italian and
         Indonesian are a second layer, NOT exhaustive, and ignore negation.
      3. The text is normalised (NFKC, zero-width characters, diacritics, look-alike
         letters, leetspeak, letters spaced or dotted apart, fragments re-joined into
         a known word) in several variants; any unsafe variant blocks.
      4. Per clause: a cue (raise / comparative / "thêm" / set-to-a-value / add dB /
         disable / bypass / remove / clear-a-notch) within a few words of a risk
         object blocks, in either order. A cue with no object of its own inherits one
         from the previous clauses of the paragraph (and "it", "nó", "chúng", "them").
      5. Default-deny: a clause naming gain, volume, âm lượng, độ lợi, fader, trim,
         preamp, master or the depth ceiling (trần) blocks unless it carries an
         explicit reduce cue (giảm, hạ, bớt, lower, cut, ...) or only raise cues that
         a plain negation governs. Neutral mentions are blocked on purpose.
         "độ lợi vòng", "loop gain" and "chạm trần" (the app's own findings) are exempt,
         and so is a statement: a figure or a status ("hiện tại", "ổn", "looks fine",
         "giữ nguyên") with no advice word and no raise cue.
      6. Also blocked: making a notch shallower (nông hơn, shallower, giảm độ sâu);
         the app's indirect names for the protection (tính năng, chế độ tự động, AUTO,
         bộ triệt hú, plugin) and for notches (điểm cắt, bộ lọc, filter); a bare
         "up"/"lên" next to a level object or carrier (kênh, bus, aux, wedge, giọng ...).
         "tốc độ tăng"/"mức tăng" are nouns and "dB mỗi giây" is a rate, not advice.
    A plain negation directly in front of the cue ("đừng tăng", "never raise")
    excuses it; rhetorical or double negation ("sao không tăng gain?", "don't hesitate
    to push the fader") does not. A past-tense report of fact ("notch 3 đã bị xoá",
    "notch sâu thêm 6 dB") is not advice.

    This is a phrase filter, not a guarantee: a model can still say the same thing
    in words these lists do not contain. It is deliberately wide: a false block costs
    one fixed sentence on screen, a false pass can reach a PA system. The system
    prompt and the app's own clamps remain the other layers."""
    if _foreign_script(text) or not _language_ok(text):
        return True
    has_leet = any(c.isdigit() or c in "@$" for c in text)
    for para in re.split(r"\n\s*\n", text):
        for squeeze in (False, True):
            for glue in (True, False):
                for leet in ((False, True) if has_leet else (False,)):
                    if _text_violates(_prep(para, glue, squeeze, leet)):
                        return True
    return False


# --- anthropic adapter -------------------------------------------------------

def build_anthropic_kwargs(model: str, tools: list, question: str,
                           system: str = SYSTEM_PROMPT,
                           max_iterations: int = MAX_ITERATIONS) -> dict:
    """The exact keyword arguments handed to `tool_runner` (pure; sends nothing)."""
    kwargs: dict = {
        "model": model,
        "max_tokens": 16000,
        "thinking": {"type": "adaptive"},
        "output_config": {"effort": "medium"},
        "system": system,
        "tools": tools,
        "max_iterations": max_iterations,
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


_NO_CREDENTIALS_MESSAGE = ("Không có thông tin đăng nhập Claude API (lưu khoá trong app, "
                           "ANTHROPIC_API_KEY, hoặc `ant auth login`).")


ANTHROPIC_API_BASE_URL = "https://api.anthropic.com"
# Read by the SDK (anthropic/_client.py) for the host, request headers and the
# profile / workload-identity credential sources.
_ANTHROPIC_STEERING_ENV = ("ANTHROPIC_BASE_URL", "ANTHROPIC_CUSTOM_HEADERS", "ANTHROPIC_PROFILE",
                           "ANTHROPIC_IDENTITY_TOKEN", "ANTHROPIC_IDENTITY_TOKEN_FILE",
                           "ANTHROPIC_FEDERATION_RULE_ID", "ANTHROPIC_ORGANIZATION_ID")


def _load_anthropic():
    """The `anthropic` module. The one seam tests replace with a fake module."""
    import anthropic
    return anthropic


def _anthropic_supervise_tools(ts: "ToolSet") -> list[dict]:
    return [{"name": n, "description": sp["description"], "input_schema": sp["parameters"]}
            for n, sp in ts.specs.items()]


def _block_get(block: object, name: str, default: object = None) -> object:
    if isinstance(block, dict):
        return block.get(name, default)
    return getattr(block, name, default)


def _run_anthropic_supervise(anthropic, client, model: str, question: str, timeout: float,
                             ts: "ToolSet", key: str | None) -> dict:
    """The supervise loop on Anthropic: manual `messages.create`, one request at a
    time, each under the time left (like the openai_compatible loop). Recording
    stops the moment the model makes no tool call."""
    tools = _anthropic_supervise_tools(ts)
    messages: list[dict] = [{"role": "user", "content": question}]
    inp: list = []
    outp: list = []

    def usage() -> dict:
        return {"input_tokens": _sum_usage(inp), "output_tokens": _sum_usage(outp)}

    for _ in range(ts.max_iterations):
        try:
            req_timeout = ts.request_timeout(timeout)
        except LoopStopped as stop:
            stop.usage = usage()
            raise
        kwargs: dict = {"model": model, "max_tokens": 16000, "thinking": {"type": "adaptive"},
                        "output_config": {"effort": "medium"}, "system": ts.system,
                        "tools": tools, "messages": messages, "timeout": req_timeout}
        create = client.messages.create
        if model in FALLBACK_MODELS:
            kwargs["betas"] = ["server-side-fallback-2026-07-01"]
            kwargs["fallbacks"] = "default"
            create = client.beta.messages.create
        try:
            resp = create(**kwargs)
        except anthropic.APITimeoutError:
            raise ProviderUnavailable("deadline: Claude API request timed out.", usage()) from None
        except anthropic.APIConnectionError as exc:
            raise ProviderUnavailable(redact(f"Không kết nối được Claude API: {exc}", key),
                                      usage()) from None
        u = getattr(resp, "usage", None)
        inp.append(getattr(u, "input_tokens", None))
        outp.append(getattr(u, "output_tokens", None))
        if getattr(resp, "stop_reason", None) == "refusal":
            raise AskError("Claude từ chối trả lời câu này.")
        content = list(getattr(resp, "content", None) or [])
        calls = [b for b in content if _block_get(b, "type") == "tool_use"]
        if not calls:
            return {"answer": "", "usage": usage()}
        messages.append({"role": "assistant", "content": content})
        results = []
        for call in calls:
            result = run_tool(ts.funcs, _block_get(call, "name"), _block_get(call, "input"), ts.specs)
            results.append({"type": "tool_result", "tool_use_id": _block_get(call, "id"),
                            "content": result})
        messages.append({"role": "user", "content": results})
    raise LoopStopped(f"tool loop exhausted: mô hình gọi công cụ quá {ts.max_iterations} lượt "
                      "mà chưa kết thúc.", usage())


def run_anthropic(events: list[dict], question: str, model: str, timeout: float,
                  credentials_only: bool = False, toolset: "ToolSet | None" = None) -> dict:
    try:
        anthropic = _load_anthropic()
    except ImportError:
        raise AskError("--ask cần gói `anthropic` (pip install anthropic).") from None

    key = resolve_key("anthropic", None, credentials_only)
    if credentials_only:
        # Only the stored key may be used: keep the SDK from resolving another
        # credential (it reads these two from the environment by itself).
        for name in ("ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN"):
            leaked = os.environ.pop(name, None)
            if leaked:
                _SECRETS.add(leaked)   # still redacted from any error text
        # The SDK also reads its host and extra headers from the environment: an
        # inherited ANTHROPIC_BASE_URL would send the stored key to another host.
        for name in _ANTHROPIC_STEERING_ENV:
            os.environ.pop(name, None)
        if not key:
            raise AskError(_NO_CREDENTIALS_MESSAGE)
    last = None
    inp: list = []
    outp: list = []
    deadline_hit = False
    try:
        if toolset is not None:
            # Raises LoopStopped when the budget is already spent. The SDK client's
            # timeout is fixed per client, so the first request gets the shrunk value.
            timeout = toolset.request_timeout(timeout)
        extra: dict = {} if toolset is None else {"max_retries": 0}   # supervise: no stacked retries
        if credentials_only:
            client = anthropic.Anthropic(api_key=key, timeout=timeout,
                                         base_url=ANTHROPIC_API_BASE_URL, **extra)
        else:
            client = anthropic.Anthropic(api_key=key, timeout=timeout, **extra) if key \
                else anthropic.Anthropic(timeout=timeout, **extra)
        if toolset is not None:
            return _run_anthropic_supervise(anthropic, client, model, question, timeout,
                                            toolset, key)
        if toolset is None:
            kwargs = build_anthropic_kwargs(model, make_tools(events), question)
        else:
            kwargs = build_anthropic_kwargs(model, make_toolset_tools(toolset), question,
                                            toolset.system, toolset.max_iterations)
        runner = client.beta.messages.tool_runner(**kwargs)
        for message in runner:
            last = message
            u = getattr(message, "usage", None)
            inp.append(getattr(u, "input_tokens", None))
            outp.append(getattr(u, "output_tokens", None))
            if toolset is not None and toolset.deadline_at is not None \
                    and getattr(message, "stop_reason", None) == "tool_use" \
                    and toolset.deadline_at - _now() < SUPERVISE_MIN_REMAINING_S:
                deadline_hit = True     # do not let the runner start another request
                break
    except (anthropic.AuthenticationError, TypeError):
        # TypeError: the SDK raises it, not an API error, when it finds no
        # credential source at all (no key, no token, no `ant` profile).
        raise AskError(_NO_CREDENTIALS_MESSAGE) from None
    except anthropic.APIConnectionError as exc:
        raise AskError(redact(f"Không kết nối được Claude API: {exc}", key)) from None
    except anthropic.APIStatusError as exc:
        raise AskError(redact(f"Claude API trả lỗi {exc.status_code}: {exc.message}", key)) from None

    if last is None:
        raise AskError("Claude không trả lời.")
    if last.stop_reason == "refusal":
        raise AskError("Claude từ chối trả lời câu này.")
    allow_empty = toolset is not None and toolset.allow_empty_reply
    usage = {"input_tokens": _sum_usage(inp), "output_tokens": _sum_usage(outp)}
    if deadline_hit:
        raise LoopStopped("deadline: hết thời gian cho lượt giám sát.", usage)
    if allow_empty and last.stop_reason == "tool_use":
        # The runner stopped at its iteration cap with a tool call still pending.
        cap = toolset.max_iterations if toolset is not None else MAX_ITERATIONS
        raise LoopStopped(f"tool loop exhausted: mô hình gọi công cụ quá {cap} lượt "
                          "mà chưa kết thúc.", usage)
    answer = "".join(b.text for b in last.content if b.type == "text")
    if not answer.strip() and not allow_empty:
        raise AskError("Claude không trả lời.")
    return {"answer": answer, "usage": {"input_tokens": _sum_usage(inp),
                                        "output_tokens": _sum_usage(outp)}}


# --- openai_compatible adapter (stdlib only) ---------------------------------

class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Never follow a redirect: the Authorization header must not travel to
    another host. A 3xx surfaces as an HTTPError."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D401
        return None


def _openai_tools(specs: dict | None = None) -> list[dict]:
    return [{"type": "function",
             "function": {"name": n, "description": sp["description"], "parameters": sp["parameters"]}}
            for n, sp in (TOOL_SPECS if specs is None else specs).items()]


def _build_opener(url: str):
    """No redirects, and no proxy at all for an http:// URL: the bearer key would
    otherwise travel in cleartext to whatever HTTP proxy the environment names.
    An https:// URL keeps the system proxy (a CONNECT tunnel; some sites need it)."""
    handlers: list = [_NoRedirect]
    if urllib.parse.urlsplit(url).scheme.lower() == "http":
        handlers.append(urllib.request.ProxyHandler({}))
    return urllib.request.build_opener(*handlers)


def _post_json(url: str, payload: dict, key: str | None, timeout: float) -> dict:
    headers = {"Content-Type": "application/json", "Accept": "application/json"}
    if key:
        headers["Authorization"] = f"Bearer {key}"
    req = urllib.request.Request(url, data=json.dumps(payload).encode("utf-8"),
                                 headers=headers, method="POST")
    opener = _build_opener(url)
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
        raise ProviderUnavailable(f"Quá hạn {timeout:g} s chờ máy chủ.") from None
    except urllib.error.URLError as exc:
        if isinstance(exc.reason, (TimeoutError, socket.timeout)):
            raise ProviderUnavailable(f"Quá hạn {timeout:g} s chờ máy chủ.") from None
        raise ProviderUnavailable(redact(f"Không kết nối được máy chủ: {exc.reason}", key)) from None
    except (OSError, http.client.HTTPException) as exc:
        raise ProviderUnavailable(redact(f"Lỗi kết nối: {type(exc).__name__}", key)) from None
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
                          key: str | None, timeout: float,
                          toolset: "ToolSet | None" = None) -> dict:
    _refuse_plaintext_key(base_url, key)
    url = base_url.rstrip("/") + "/chat/completions"
    if toolset is None:
        funcs, specs, system, allow_empty = make_tool_functions(events), TOOL_SPECS, SYSTEM_PROMPT, False
    else:
        funcs, specs, system, allow_empty = (toolset.funcs, toolset.specs, toolset.system,
                                             toolset.allow_empty_reply)
    tools = _openai_tools(specs)
    messages: list[dict] = [{"role": "system", "content": system},
                            {"role": "user", "content": question}]
    tokens_in: list = []
    tokens_out: list = []
    cap = MAX_ITERATIONS if toolset is None else toolset.max_iterations
    for _ in range(cap):
        try:
            req_timeout = timeout if toolset is None else toolset.request_timeout(timeout)
        except LoopStopped as stop:
            stop.usage = {"input_tokens": _sum_usage(tokens_in),
                          "output_tokens": _sum_usage(tokens_out)}
            raise
        try:
            doc = _post_json(url, {"model": model, "messages": messages, "tools": tools,
                                   "tool_choice": "auto"}, key, req_timeout)
        except ProviderUnavailable as exc:
            if toolset is None:
                raise
            raise ProviderUnavailable(
                f"deadline: {exc}", {"input_tokens": _sum_usage(tokens_in),
                                     "output_tokens": _sum_usage(tokens_out)}) from None
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
                result = run_tool(funcs, fn.get("name"), fn.get("arguments"), specs)
                messages.append({"role": "tool", "tool_call_id": str(call.get("id", f"call_{i}")),
                                 "content": result})
            continue
        answer = _message_text(msg.get("content"))
        if not answer.strip() and not allow_empty:
            raise AskError("Mô hình không trả lời.")
        return {"answer": answer, "usage": {"input_tokens": _sum_usage(tokens_in),
                                            "output_tokens": _sum_usage(tokens_out)}}
    exhausted = f"tool loop exhausted: mô hình gọi công cụ quá {cap} lượt mà chưa trả lời."
    if toolset is None:
        raise AskError(exhausted)
    raise LoopStopped(exhausted, {"input_tokens": _sum_usage(tokens_in),
                                  "output_tokens": _sum_usage(tokens_out)})


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
    return validate_base_url(base_url)


def validate_base_url(base_url: str) -> str | None:
    """An error message for a bad --base-url, else None. Never echoes the URL."""
    try:
        parts = urllib.parse.urlsplit(base_url)
        parts.port  # noqa: B018 -- raises ValueError on a bad port
        userinfo = parts.username is not None or parts.password is not None or "@" in parts.netloc
    except ValueError:
        return "--base-url is not a valid URL"
    if parts.scheme not in ("http", "https") or not parts.netloc:
        return "--base-url must be an http:// or https:// URL"
    if userinfo:
        return "--base-url must not contain credentials (user:password@host)"
    if origin_of(base_url) is None:
        return "--base-url is not a valid URL"
    return None


def ask(events: list[dict], question: str, provider: str, model: str,
        base_url: str | None, timeout: float, credentials_only: bool = False) -> dict:
    """Run one question. Returns {answer, usage}; raises AskError."""
    if provider == "anthropic":
        return run_anthropic(events, question, model, timeout, credentials_only)
    return run_openai_compatible(events, question, model, base_url or "",
                                 resolve_key("openai_compatible", base_url, credentials_only),
                                 timeout)


def supervise(events: list[dict], state: dict, provider: str, model: str,
              base_url: str | None, timeout: float, credentials_only: bool = False,
              deadline_s: float = SUPERVISE_DEADLINE_DEFAULT_S) -> dict:
    """One supervisor turn. Returns the stdout object; raises AskError. The
    model's own text is dropped here: only what the recording tools queued leaves.
    The loop runs under a monotonic deadline of `deadline_s`; when it stops early
    (deadline or iteration cap) the actions already recorded are still returned,
    and with none recorded the stop is an error (exit 3)."""
    ts = build_supervise_toolset(events, state)
    ts.deadline_at = _now() + deadline_s
    try:
        if provider == "anthropic":
            res = run_anthropic(events, SUPERVISE_USER_PROMPT, model, timeout, credentials_only, ts)
        else:
            res = run_openai_compatible(events, SUPERVISE_USER_PROMPT, model, base_url or "",
                                        resolve_key("openai_compatible", base_url, credentials_only),
                                        timeout, ts)
    except LoopStopped as stop:
        if not ts.supervisor.actions:
            raise
        res = {"usage": stop.usage}
    return {"supervise": True, "generation": state["generation"],
            "actions": ts.supervisor.result_actions(), "provider": provider, "model": model,
            "usage": res["usage"]}


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
            store_key(args.provider, key, origin_of(args.base_url)
                      if args.provider == "openai_compatible" else None)
            print(f"Đã lưu khoá cho {args.provider}.")
            return EXIT_OK
        if args.delete_key:
            delete_key(args.provider)
            print(f"Đã xoá khoá của {args.provider}.")
            return EXIT_OK
        # --has-key: for openai_compatible, answers for the origin of --base-url
        # (required; validated in _main).
        return EXIT_OK if load_key(args.provider, args.base_url) else 1
    except CredentialError as exc:
        print(redact(f"Lỗi kho khoá: {exc}"), file=sys.stderr)
        return EXIT_ASK_FAILED
    except OSError as exc:
        print(f"Lỗi kho khoá: {type(exc).__name__}", file=sys.stderr)
        return EXIT_ASK_FAILED


def _emit_error(args, message: str, code: int = EXIT_ASK_FAILED) -> int:
    """An --ask / --supervise failure: the redacted reason on stderr, and with
    --json the same reason as the one JSON object on stdout."""
    message = redact(message)
    print(message, file=sys.stderr)
    if args.json:
        print(json.dumps({"error": message}, ensure_ascii=False))
    return code


def main(argv: list[str]) -> int:
    """Exit 1 is reachable only from --has-key: anything unexpected is exit 3."""
    try:
        return _main(argv)
    except Exception as exc:  # noqa: BLE001 -- SystemExit/KeyboardInterrupt are not Exceptions
        name = type(exc).__name__   # the type only: str(exc) could carry a key or a path
        print(f"Lỗi không lường trước: {name}", file=sys.stderr)
        if "--json" in argv and any(a in ("--ask", "--supervise") or a.startswith("--ask=")
                                    for a in argv):
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
    ap.add_argument("--supervise", action="store_true",
                    help="machine mode: the model may only call recording tools over --state "
                         "(needs --json); stdout is one JSON object")
    ap.add_argument("--state", type=Path, metavar="PATH",
                    help="with --supervise: the app's state file (UTF-8 JSON, schema 2)")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S, metavar="SECONDS")
    ap.add_argument("--deadline-s", type=int, default=None, metavar="SECONDS",
                    help=f"with --supervise: wall-clock budget of the whole run, "
                         f"{SUPERVISE_DEADLINE_MIN_S}..{SUPERVISE_DEADLINE_MAX_S} "
                         f"(default {SUPERVISE_DEADLINE_DEFAULT_S})")
    ap.add_argument("--store-key", action="store_true",
                    help="read an API key from stdin and store it (DPAPI)")
    ap.add_argument("--delete-key", action="store_true")
    ap.add_argument("--has-key", action="store_true",
                    help="exit 0 if a key is stored for --provider, else 1")
    ap.add_argument("--credentials-only", action="store_true",
                    help="with --ask: use only the stored key (never OPENAI_API_KEY / "
                         "ANTHROPIC_API_KEY / ANTHROPIC_AUTH_TOKEN); the app always passes it")
    ap.add_argument("--low-priority", action="store_true",
                    help="lower the process priority (Windows); output is unchanged")
    args = ap.parse_args(argv)
    if args.low_priority:
        _lower_priority()

    key_ops = sum((args.store_key, args.delete_key, args.has_key))
    if key_ops:
        if key_ops > 1 or args.ask is not None or args.supervise:
            ap.error("--store-key, --delete-key, --has-key, --ask and --supervise "
                     "are mutually exclusive")
        if args.credentials_only:
            ap.error("--credentials-only only applies with --ask or --supervise")
        if args.state is not None:
            ap.error("--state only applies with --supervise")
        if args.deadline_s is not None:
            ap.error("--deadline-s only applies with --supervise")
        if (args.store_key or args.has_key) and args.provider == "openai_compatible":
            if not args.base_url:
                ap.error("--base-url is required to store or check an openai_compatible key")
            problem = validate_base_url(args.base_url)
            if problem:
                ap.error(problem)
        return _key_operation(args)
    if args.file is None:
        ap.error("the log file is required")

    model = args.model
    if args.supervise:
        if args.ask is not None:
            ap.error("--ask and --supervise are mutually exclusive")
        if args.state is None:
            ap.error("--supervise needs --state=<path>")
        if not args.json:
            ap.error("--supervise needs --json")
        if args.deadline_s is not None and not (
                SUPERVISE_DEADLINE_MIN_S <= args.deadline_s <= SUPERVISE_DEADLINE_MAX_S):
            ap.error(f"--deadline-s must be in {SUPERVISE_DEADLINE_MIN_S}..{SUPERVISE_DEADLINE_MAX_S}")
    elif args.state is not None:
        ap.error("--state only applies with --supervise")
    elif args.deadline_s is not None:
        ap.error("--deadline-s only applies with --supervise")
    if args.ask is not None or args.supervise:
        if args.provider == "anthropic" and not model:
            model = DEFAULT_ANTHROPIC_MODEL
        problem = validate_ask_args(args.provider, model, args.base_url, args.timeout)
        if problem:
            ap.error(problem)
    elif args.model or args.base_url or args.credentials_only:
        ap.error("--model, --base-url and --credentials-only only apply with --ask or --supervise")

    def bad_input(message: str) -> int:
        # --supervise is machine-read: stdout is always one JSON object, even here.
        if args.supervise:
            return _emit_error(args, message, EXIT_BAD_LOG)
        print(message, file=sys.stderr)
        return EXIT_BAD_LOG

    try:
        events = logstats.load(args.file)
        if any(not isinstance(e, dict) for e in events):
            raise ValueError("a log line is not a JSON object")
        fs = findings(events) if events else []
    except OSError as exc:
        return bad_input(f"empty or unreadable log: {exc}")
    except Exception as exc:  # noqa: BLE001 -- undecodable / malformed log: exit 2, type only
        return bad_input(f"unreadable log: {type(exc).__name__}")
    if not events:
        return bad_input("empty or unreadable log")

    if args.supervise:
        try:
            state = load_state(args.state)
        except StateError as exc:
            return _emit_error(args, str(exc), EXIT_BAD_STATE)
        try:
            result = supervise(events, state, args.provider, model, args.base_url, args.timeout,
                               args.credentials_only,
                               SUPERVISE_DEADLINE_DEFAULT_S if args.deadline_s is None
                               else args.deadline_s)
        except AskError as exc:
            return _emit_error(args, str(exc))
        except Exception as exc:  # noqa: BLE001 -- last resort; type name only, never the message
            return _emit_error(args, f"Lỗi không lường trước trong trợ lý ({type(exc).__name__})")
        print(json.dumps(result, ensure_ascii=False))
        return EXIT_OK

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
        result = ask(events, args.ask, args.provider, model, args.base_url, args.timeout,
                     args.credentials_only)
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
