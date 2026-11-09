#!/usr/bin/env python3
"""Print the Python half of installer/THIRD-PARTY-NOTICES.txt to stdout.

It reads the interpreter's own environment as given (run it with the build venv's
python, e.g. build/copilot/venv/Scripts/python.exe -I tools/copilot/collect_licenses.py)
and prints, for CPython and for every distribution bundled into HandsFreeCopilot.exe:
name, version, licence identifier and the licence files copied verbatim from the
interpreter's LICENSE.txt / the distribution's *.dist-info. It sends nothing anywhere.

The bundled set is the set of top-level packages PyInstaller put in the PYZ archive
(build/copilot/work/HandsFreeCopilot/PYZ-00.toc) plus PyInstaller itself (its
bootloader is the sidecar's exe). A distribution missing from the environment is
reported on stderr and exits 1: a notices file with a silent hole is worse than none.
"""
from __future__ import annotations

import sys
from importlib import metadata
from pathlib import Path

BUNDLED = (
    "anthropic", "anyio", "annotated-types", "docstring-parser", "h11", "httpcore2",
    "httpx2", "idna", "jiter", "packaging", "pydantic", "pydantic-core", "setuptools",
    "sniffio", "truststore", "typing-extensions", "typing-inspection", "pyinstaller",
)
LICENCE_PREFIXES = ("LICENSE", "LICENCE", "COPYING", "NOTICE")
RULE = "=" * 78


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n").strip("\n")


def cpython() -> dict:
    lic = Path(sys.base_prefix) / "LICENSE.txt"
    if not lic.is_file():
        raise FileNotFoundError(f"CPython licence not found: {lic}")
    v = sys.version_info
    return {
        "name": "CPython",
        "version": f"{v.major}.{v.minor}.{v.micro}",
        "licence": "PSF-2.0",
        "texts": [(lic.name, _read(lic))],
    }


def distribution(name: str) -> dict:
    dist = metadata.distribution(name)
    meta = dist.metadata
    licence = meta.get("License-Expression") or meta.get("License") or "UNKNOWN"
    texts: list[tuple[str, str]] = []
    seen: set[str] = set()
    for f in dist.files or []:
        if not f.name.upper().startswith(LICENCE_PREFIXES):
            continue
        if ".dist-info" not in str(f.parent):
            continue
        body = _read(Path(dist.locate_file(f)))
        if body and body not in seen:
            seen.add(body)
            texts.append((f.name, body))
    if not texts:
        raise FileNotFoundError(f"no licence file in the metadata of {name}")
    return {"name": meta["Name"], "version": meta["Version"], "licence": licence, "texts": texts}


def components() -> list[dict]:
    return [cpython()] + [distribution(n) for n in BUNDLED]


def render(c: dict) -> str:
    out = [RULE, f"{c['name']} {c['version']}", f"Licence: {c['licence']}", RULE, ""]
    for fname, body in c["texts"]:
        out += [f"--- {fname} ---", "", body, ""]
    return "\n".join(out)


def main() -> int:
    try:
        comps = components()
    except (metadata.PackageNotFoundError, FileNotFoundError) as exc:
        print(f"collect_licenses: {exc}", file=sys.stderr)
        return 1
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    print("\n".join(render(c) for c in comps))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
