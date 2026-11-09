#!/usr/bin/env python3
"""ctest gate: every model the app embeds is either a placeholder or gated.

Standard library only. For every weights JSON directly under the models
directory (default: <repo>/models, the folder juce_add_binary_data embeds):

  * its "model" id starts with "untrained-"  -> allowed (a placeholder whose
    score means nothing, and the logs say so), or
  * a sibling <stem>.eval-report.json exists and tools/nn/gate.py passes on it
    against THIS weights file (sha256 match) -> allowed.

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


def check_models_dir(models_dir):
    """Returns a list of problems (empty = every embedded model is allowed)."""
    problems = []
    try:
        names = sorted(n for n in os.listdir(models_dir) if n.endswith(".json")
                       and not n.endswith(".eval-report.json") and not n.endswith(".manifest.json"))
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
            continue
        stem = n[:-5]
        report = os.path.join(models_dir, stem + ".eval-report.json")
        if not os.path.isfile(report):
            problems.append("%s: model '%s' is not an untrained-* placeholder and has no %s.eval-report.json"
                            % (n, model_id, stem))
            continue
        manifest = os.path.join(models_dir, stem + ".manifest.json")
        fails = gate.check_files(report, path, manifest if os.path.isfile(manifest) else None)
        for f in fails:
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
