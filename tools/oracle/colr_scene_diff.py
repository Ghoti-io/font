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
"""COLR lowered to a scene (stories 14c and P4), this library against a lowering from fontTools.

documentation/design.md section 8.8. For every font with a `COLR` table - the
committed fixtures, the image's color fonts and the pinned real color fonts the
fetcher stages - `font-colr-scene` prints what `gfnt_scene_record_color_glyph`
records for each color glyph (or why it refused it), and
`fonttools_colr_scene.py` states the same rules L1 to L11 a second time over
fontTools' own decompilation, with fontTools' own transforms. Every glyph must
agree: the source, the refusal and its paint format and mode, and item by item
the kind, the glyph, the paint, the entry, the alpha and the operator exactly,
the maps within 1e-9 of each entry's magnitude (or of 1), since a skew is a
tangent fontTools computes with `tan` and this library as a sine over a cosine,
and, for each item's extra (a gradient's kind, extend, space, premultiplied
flag, geometry, map and every stop; a group's blend space; a clip box's
rectangle), exactly, in integers or the doubles they are exact in.

**Passes.** The standard's space and the encoded one (`GFNT_COLOR_SPACE_ENCODED`),
each at the default instance; and for a variable font the instances of
`INSTANCES`, each named by the normalised coordinates (2.14 integers) the face
resource is interned at (L1) and handed to the oracle, which resolves every
`Var*` number with fontTools' own item variation store at the same point. The
first instances are binary fractions of the axis range, where a delta is a
dyadic number and the two sides round it alike; the last is not, and there the
two sides may differ by one unit of a value where this library's rounding of a
delta crosses a tie (the documented limit of `check-oracle-color`): a glyph that
differs only within that bound is counted as the tie class and not failed.

The report counts the glyphs lowered and refused by reason, the items, the
gradients and planes among them, and the tie class, so a run that lowered
nothing would read as such.

Usage:
    colr_scene_diff.py [--quiet] [font...]
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import colr_real_fonts
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-colr-scene")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")
SCRATCH = os.path.join(ROOT, "build", "oracle", "colr-scene")
REASONS = {0: "lowered", 1: "unbounded", 2: "no record"}

# Instances of a variable font, as fractions of the first axes' ranges in 2.14 (16384 is 1.0). The first three are
# binary fractions; the last is not.
INSTANCES = [
    ("quarter", [4096, -4096]),
    ("half", [8192, 8192]),
    ("corner", [16384, -16384]),
    ("general", [5461, -3001]),
]
TIE_INSTANCE = "general"


def has_colr(path):
    try:
        with open(path, "rb") as handle:
            head = handle.read(12)
            count = int.from_bytes(head[4:6], "big")
            tags = {handle.read(16)[:4] for _ in range(count)}
    except (OSError, ValueError):
        return False
    return b"COLR" in tags


def has_fvar(path):
    try:
        with open(path, "rb") as handle:
            head = handle.read(12)
            count = int.from_bytes(head[4:6], "big")
            tags = {handle.read(16)[:4] for _ in range(count)}
    except (OSError, ValueError):
        return False
    return b"fvar" in tags


def close(a, b, tolerance=1e-9):
    return abs(a - b) <= tolerance * max(1.0, abs(a), abs(b))


class Ties:
    """The class of difference the tie instance allows: a number one unit of its own scale away."""

    def __init__(self, allowed):
        self.allowed = allowed
        self.seen = 0

    def number(self, a, b, unit):
        if close(a, b):
            return True
        if self.allowed and abs(a - b) <= unit * 1.0000001:
            self.seen += 1
            return True
        return False


def differ_extra(a, b, ties):
    if a is None and b is None:
        return None
    if (a is None) != (b is None):
        return "extra: this library %r, fontTools %r" % (a, b)
    if set(a) != set(b):
        return "extra keys: this library %r, fontTools %r" % (sorted(a), sorted(b))
    if "space" in a and a["space"] != b["space"]:
        return "group space: this library %r, fontTools %r" % (a["space"], b["space"])
    if "box" in a:
        for k in range(4):
            if not ties.number(a["box"][k], b["box"][k], 1.0):
                return "clip box %d: this library %r, fontTools %r" % (k, a["box"][k], b["box"][k])
    if "g" in a:
        x, y = a["g"], b["g"]
        for key in ("kind", "extend", "space", "premul"):
            if x[key] != y[key]:
                return "gradient %s: this library %r, fontTools %r" % (key, x[key], y[key])
        if len(x["geo"]) != len(y["geo"]):
            return "gradient geometry length"
        for k, (p, q) in enumerate(zip(x["geo"], y["geo"])):
            if not ties.number(p, q, 1.0):
                return "gradient geometry %d: this library %r, fontTools %r" % (k, p, q)
        for k in range(6):
            if not ties.number(x["map"][k], y["map"][k], 1e-3 * max(1.0, abs(y["map"][k]))
                               if ties.allowed else 0.0):
                return "gradient map %d: this library %r, fontTools %r" % (k, x["map"][k], y["map"][k])
        if len(x["stops"]) != len(y["stops"]):
            return "gradient stops: this library %d, fontTools %d" % (len(x["stops"]), len(y["stops"]))
        for k, (s, t) in enumerate(zip(x["stops"], y["stops"])):
            if s[1] != t[1] or s[2] != t[2]:
                return "stop %d paint: this library %r, fontTools %r" % (k, s[1:3], t[1:3])
            if not ties.number(s[0], t[0], 1 / 16384) or not ties.number(s[3], t[3], 1 / 16384):
                return "stop %d: this library %r, fontTools %r" % (k, s, t)
    return None


def differ(ours, ref, ties):
    """The first difference between two glyph answers, or None."""
    for key in ("error", "src", "refusal", "format", "mode"):
        if ours.get(key) != ref.get(key):
            return "%s: this library %r, fontTools %r" % (key, ours.get(key), ref.get(key))
    a = ours.get("items", [])
    b = ref.get("items", [])
    if len(a) != len(b):
        return "items: this library %d, fontTools %d" % (len(a), len(b))
    for i, (x, y) in enumerate(zip(a, b)):
        if x[:4] != y[:4] or x[5] != y[5]:
            return "item %d: this library %r, fontTools %r" % (i, x[:6], y[:6])
        if not ties.number(x[4], y[4], 1 / 16384):
            return "item %d alpha: this library %r, fontTools %r" % (i, x[4], y[4])
        for k in range(6, 12):
            if not ties.number(x[k], y[k], 1e-3 * max(1.0, abs(y[k])) if ties.allowed else 0.0):
                return "item %d map entry %d: this library %r, fontTools %r" % (i, k - 6, x[k], y[k])
        where = differ_extra(x[12], y[12], ties)
        if where:
            return "item %d %s" % (i, where)
    return None


def run_ours(font, encoded, coords):
    argv = [DRIVER, font] + (["--encoded"] if encoded else []) + [str(c) for c in (coords or [])]
    run = subprocess.run(argv, capture_output=True, text=True)
    try:
        return json.loads(run.stdout)["glyphs"]
    except (ValueError, KeyError):
        return {"*": {"error": run.stderr.strip()[:200] or "no answer"}}


def run_reference(fonts, encoded, coords_by_font, name):
    os.makedirs(SCRATCH, exist_ok=True)
    out = os.path.join(SCRATCH, "fonttools-%s.json" % name)
    coords_path = os.path.join(SCRATCH, "coords-%s.json" % name)
    with open(coords_path, "w", encoding="utf-8") as handle:
        json.dump(coords_by_font, handle)
    inner = ["python3", os.path.join(ROOT, "tools", "oracle", "fonttools_colr_scene.py"), "--coords", coords_path]
    if encoded:
        inner.append("--encoded")
    argv_ref = oracle_env.command("fonttools", inner + [out] + fonts, scratch=SCRATCH)
    finished = subprocess.run(argv_ref, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable("fonttools_colr_scene failed: %s" % finished.stderr.strip()[-300:])
    with open(out, encoding="utf-8") as handle:
        return json.load(handle)


def count_items(items, stats):
    for item in items:
        stats["items"] += 1
        if item[1] == -2:
            stats["planes"] += 1
        if item[1] == -3:
            stats["boxes"] += 1
        extra = item[12]
        if extra and "g" in extra:
            stats["gradients"] += 1


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
        try:
            pinned = colr_real_fonts.ensure()
        except colr_real_fonts.FetchFailed as why:
            raise oracle_env.OracleUnavailable("the pinned real color fonts: %s" % why)
        fonts = fixtures + corpus_fonts + pinned
    passes = [("standard", False, None), ("encoded", True, None)]
    for name, coords in INSTANCES:
        passes.append((name, False, coords))
    failed = 0
    glyphs = 0
    stats = {"items": 0, "planes": 0, "boxes": 0, "gradients": 0}
    by_reason = {}
    tie_glyphs = 0
    for name, encoded, coords in passes:
        variable = [f for f in fonts if has_fvar(f)] if coords else fonts
        if not variable:
            continue
        coords_by_font = {f: coords for f in variable} if coords else {}
        ours = {f: run_ours(f, encoded, coords) for f in variable}
        want = run_reference(variable, encoded, coords_by_font, name)
        for font in variable:
            got = ours[font]
            ref = want[font]["glyphs"]
            for gid in sorted(set(got) | set(ref), key=lambda g: (len(g), g)):
                a = got.get(gid, {"missing": True})
                b = ref.get(gid, {"missing": True})
                if name == "standard":
                    glyphs += 1
                    reason = "limit" if "error" in b else REASONS.get(b.get("refusal"), "?")
                    by_reason[reason] = by_reason.get(reason, 0) + 1
                    count_items(b.get("items", []), stats)
                ties = Ties(name == TIE_INSTANCE)
                where = differ(a, b, ties) if "missing" not in a and "missing" not in b else "only one side lowers it"
                if where:
                    failed += 1
                    if failed <= 40 or not quiet:
                        print("DIFF [%s] %s glyph %s: %s" % (name, os.path.relpath(font, ROOT), gid, where))
                elif ties.seen:
                    tie_glyphs += 1
    print("fonts %d (%d from the image's corpus, plus the pinned real fonts), %d color glyphs (%s), %d items (%d gradients, %d planes, %d clip boxes) "
          "in %d passes, %d differing; the tie class (one unit at the non-binary instance): %d glyphs"
          % (len(fonts), real, glyphs, ", ".join("%s %d" % kv for kv in sorted(by_reason.items())), stats["items"],
             stats["gradients"], stats["planes"], stats["boxes"], len(passes), failed, tie_glyphs))
    return 1 if failed or not fonts or glyphs == 0 else 0


if __name__ == "__main__":
    try:
        oracle_env.provenance(["fonttools"])
        sys.exit(main(sys.argv))
    except oracle_env.OracleUnavailable as why:
        sys.stderr.write("SKIPPED colr_scene_diff: %s\n" % why)
        sys.exit(1 if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1" else 0)
