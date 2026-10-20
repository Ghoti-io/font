#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
#
# Ghoti.io Font is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""COLR lowered to a scene (story 14c), this library against a lowering from fontTools.

documentation/design.md section 8.8. For every font with a `COLR` table - the
committed fixtures and the image's color fonts - `font-colr-scene` prints what
`gfnt_scene_record_color_glyph` records for each color glyph (or why it refused
it), and `fonttools_colr_scene.py` states the same rules a second time over
fontTools' own decompilation, with fontTools' own transforms. Every glyph must
agree: the source, the refusal and its paint format and mode, and item by item
the kind, the glyph, the paint, the entry, the alpha and the operator exactly,
and the map within 1e-9 of each entry's magnitude (or of 1), since a skew is a
tangent fontTools computes with `tan` and this library as a sine over a cosine.

The report counts the glyphs lowered and refused by reason, so a run that lowered
nothing would read as such.

Usage:
    colr_scene_diff.py [--quiet] [font...]
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-colr-scene")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")
SCRATCH = os.path.join(ROOT, "build", "oracle", "colr-scene")
REASONS = {0: "lowered", 1: "gradient", 2: "blend mode", 3: "variable", 4: "unbounded", 5: "no record"}


def has_colr(path):
    try:
        with open(path, "rb") as handle:
            head = handle.read(12)
            count = int.from_bytes(head[4:6], "big")
            tags = {handle.read(16)[:4] for _ in range(count)}
    except (OSError, ValueError):
        return False
    return b"COLR" in tags


def close(a, b):
    return abs(a - b) <= 1e-9 * max(1.0, abs(a), abs(b))


def differ(ours, ref):
    """The first difference between two glyph answers, or None."""
    for key in ("error", "src", "refusal", "format", "mode"):
        if ours.get(key) != ref.get(key):
            return "%s: this library %r, fontTools %r" % (key, ours.get(key), ref.get(key))
    a = ours.get("items", [])
    b = ref.get("items", [])
    if len(a) != len(b):
        return "items: this library %d, fontTools %d" % (len(a), len(b))
    for i, (x, y) in enumerate(zip(a, b)):
        if x[:6] != y[:6]:
            return "item %d: this library %r, fontTools %r" % (i, x[:6], y[:6])
        for k in range(6, 12):
            if not close(x[k], y[k]):
                return "item %d map entry %d: this library %r, fontTools %r" % (i, k - 6, x[k], y[k])
    return None


def main(argv):
    quiet = "--quiet" in argv
    rest = [a for a in argv[1:] if not a.startswith("--")]
    if rest:
        fonts = [os.path.abspath(a) for a in rest]
        real = 0
    else:
        corpus_fonts = [f for f in corpus.fonts("sfnt") if has_colr(f)]
        real = len(corpus_fonts)
        fixtures = [os.path.join(FIXTURES, n) for n in sorted(os.listdir(FIXTURES))
                    if n.endswith((".ttf", ".otf")) and has_colr(os.path.join(FIXTURES, n))]
        fonts = fixtures + corpus_fonts
    os.makedirs(SCRATCH, exist_ok=True)
    ours = {}
    for font in fonts:
        run = subprocess.run([DRIVER, font], capture_output=True, text=True)
        try:
            ours[font] = json.loads(run.stdout)["glyphs"]
        except (ValueError, KeyError):
            ours[font] = {"*": {"error": run.stderr.strip()[:200] or "no answer"}}
    out = os.path.join(SCRATCH, "fonttools.json")
    argv_ref = oracle_env.command("fonttools", [
        "python3", os.path.join(ROOT, "tools", "oracle", "fonttools_colr_scene.py"), out] + fonts,
        scratch=SCRATCH)
    finished = subprocess.run(argv_ref, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable("fonttools_colr_scene failed: %s" % finished.stderr.strip()[-300:])
    with open(out, encoding="utf-8") as handle:
        want = json.load(handle)
    failed = 0
    glyphs = 0
    items = 0
    by_reason = {}
    for font in fonts:
        got = ours[font]
        ref = want[font]["glyphs"]
        for gid in sorted(set(got) | set(ref), key=lambda g: (len(g), g)):
            glyphs += 1
            a = got.get(gid, {"missing": True})
            b = ref.get(gid, {"missing": True})
            if "error" in b:
                reason = "limit"
            else:
                reason = REASONS.get(b.get("refusal"), "?")
            by_reason[reason] = by_reason.get(reason, 0) + 1
            items += len(b.get("items", []))
            where = differ(a, b) if "missing" not in a and "missing" not in b else "only one side lowers it"
            if where:
                failed += 1
                if failed <= 40 or not quiet:
                    print("DIFF %s glyph %s: %s" % (os.path.relpath(font, ROOT), gid, where))
    print("fonts %d (%d from the image's corpus), %d color glyphs (%s), %d items, %d differing"
          % (len(fonts), real, glyphs, ", ".join("%s %d" % kv for kv in sorted(by_reason.items())), items, failed))
    return 1 if failed or not fonts or glyphs == 0 else 0


if __name__ == "__main__":
    try:
        oracle_env.provenance(["fonttools"])
        sys.exit(main(sys.argv))
    except oracle_env.OracleUnavailable as why:
        sys.stderr.write("SKIPPED colr_scene_diff: %s\n" % why)
        sys.exit(1 if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1" else 0)
