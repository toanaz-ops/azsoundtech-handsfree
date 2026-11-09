#!/usr/bin/env python3
"""ctest gate: every model the app embeds is either a placeholder or gated.

Standard library only. For every weights JSON directly under the models
directory (default: <repo>/models, the folder juce_add_binary_data embeds):

  * it is a PINNED placeholder (PLACEHOLDERS: id and weights sha256) -> allowed
    (its score means nothing, and the logs say so); any other "untrained-*" fails, or
  * the siblings <stem>.eval-report.json, <stem>.manifest.json and <stem>.meta.json
    (training metadata) exist and tools/nn/gate.py passes on them against THIS
    weights file -> allowed.

Anything else fails. The synthetic bootstrap model can never be embedded: its
report is `synthetic`, which the gate rejects (spec 8.4, decision Q25).

    python tools/nn/check_embedded_models.py [--models-dir DIR]
"""
from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gate  # noqa: E402

PLACEHOLDER_PREFIX = "untrained-"
# Known placeholders, pinned by content: {model id: sha256 of the weights file,
# CRLF read as LF}. An "untrained-*" id that is not listed here, or whose file
# differs, is NOT a placeholder -- trained weights renamed "untrained-evil" must
# not slip through. Adding a placeholder is a reviewed edit of this table.
PLACEHOLDERS = {
    "untrained-fixture-v1": "00c016c31c12e9abefe5a3ad65876374f4ec07ad13894cd088c319c4c22d15e8",
}


def check_models_dir(models_dir):
    """Returns a list of problems (empty = every embedded model is allowed)."""
    problems = []
    skip = (".eval-report.json", ".manifest.json", ".meta.json", ".parity.json")
    try:
        names = sorted(n for n in os.listdir(models_dir)
                       if n.endswith(".json") and not n.endswith(skip))
    except OSError as exc:
        return ["cannot list %s (%s)" % (models_dir, exc)]
    if not names:
        return ["no model JSON under %s (the app embeds at least the placeholder)" % models_dir]
    for n in names:
        path = os.path.join(models_dir, n)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                doc = json.load(fh)
        except (OSError, ValueError) as exc:
            problems.append("%s: unreadable (%s)" % (n, exc))
            continue
        model_id = doc.get("model") if isinstance(doc, dict) else None
        if not isinstance(model_id, str) or not model_id:
            problems.append("%s: no model id" % n)
            continue
        if model_id.startswith(PLACEHOLDER_PREFIX):
            pinned = PLACEHOLDERS.get(model_id)
            if pinned is None:
                problems.append("%s: '%s' is not a known placeholder (allowlist in check_embedded_models.py)"
                                % (n, model_id))
            elif gate.sha256_file(path) != pinned:
                problems.append("%s: placeholder '%s' has different weights than the pinned file"
                                % (n, model_id))
            continue
        stem = n[:-5]
        sibling = {k: os.path.join(models_dir, stem + suffix) for k, suffix in
                   (("report", ".eval-report.json"), ("manifest", ".manifest.json"), ("meta", ".meta.json"))}
        missing = [os.path.basename(v) for v in sibling.values() if not os.path.isfile(v)]
        if missing:
            problems.append("%s: model '%s' is not a pinned placeholder and lacks %s"
                            % (n, model_id, ", ".join(missing)))
            continue
        for f in gate.check_files(sibling["report"], path, sibling["manifest"], sibling["meta"]):
            problems.append("%s: gate: %s" % (n, f))
    return problems


def main(argv):
    default = os.path.join(HERE, "..", "..", "models")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--models-dir", default=default)
    args = ap.parse_args(argv)
    problems = check_models_dir(args.models_dir)
    if problems:
        print("EMBEDDED MODEL GATE FAILED:")
        for p in problems:
            print("  - " + p)
        return 1
    print("embedded model gate OK (%s)" % os.path.normpath(args.models_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
