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
"""`CPAL` and `COLR`, this library against fontTools.

documentation/design.md sections 7.6 and 14. For every font that has either
table - the committed color fixtures, and any color font the oracle image holds -
`font-color` prints what this library reads (every palette, every color, every
label, and the layers of every glyph that has any) and fontTools' own decompiler
is asked the same, in the fonttools image. The two JSON answers must be equal.

**The population is the fixtures and fifteen real fonts.** Debian has no font
with `COLR` or `CPAL`, so the image fetches googlefonts/color-fonts at a pinned
commit (`fonttools-fetch-color-fonts`): Google's COLR v1 test-glyph fonts, three
producers' samples in glyf, CFF and CFF2 outlines, and a full Twemoji and Noto
Emoji. This script scans the corpus for color tables, so a font that appears is
compared without an edit, and the report says how many real fonts it compared.

A variable font is read at locations made from its own axes. The test-glyph font
has 43, one per variable paint field (SWPS is a sweep's start angle, GRX0 a
gradient point, and so on), so a single axis moved to half way isolates one field
and every Var form is exercised alone; the combined locations then move them all
together.

Usage:
    color_diff.py [--quiet] [font...]
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-color")
FIXTURES = os.path.join(ROOT, "tests", "data", "fonts")
SCRATCH = os.path.join(ROOT, "build", "oracle", "color")
# Where the variable fixtures are read. The normalized coordinates are chosen to be
# binary fractions with at most two bits (`avar` sends wght 650 to 0.75, and wdth
# 87.5 and 112.5 are -0.5 and 0.5), so every delta is a product that both
# fontTools' floats and this library's 16.16 arithmetic hold exactly. At a general
# point the two differ by the 1/64 at which the library rounds a delta before its
# final rounding, which moves a result across a tie once in a long while; that is
# the library's documented arithmetic (var_diff.py holds it) and not a thing this
# gate is for.
LOCATIONS = [None, "wght=650", "wght=900", "wdth=87.5", "wdth=112.5",
             "wght=650,wdth=87.5", "wght=100,wdth=125", "wght=650,wdth=112.5"]


def axes_of(path):
    """(tag, min, default, max) of the font's fvar axes, read without fontTools."""
    import struct
    with open(path, "rb") as handle:
        data = handle.read()
    count = struct.unpack(">H", data[4:6])[0]
    for i in range(count):
        tag, _, offset, length = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        if tag == b"fvar":
            head = offset + struct.unpack(">H", data[offset + 4:offset + 6])[0]
            n = struct.unpack(">H", data[offset + 8:offset + 10])[0]
            size = struct.unpack(">H", data[offset + 10:offset + 12])[0]
            out = []
            for k in range(n):
                at = head + k * size
                t = data[at:at + 4].decode("ascii")
                lo, df, hi = (v / 65536.0 for v in struct.unpack(">iii", data[at + 4:at + 16]))
                out.append((t, lo, df, hi))
            return out
    return []


def locations_for(path):
    """The locations a font is read at: the shared list when it has wght or wdth,
    else one axis at a time to half way each side, then every axis together at
    half way and at its end. Half way is a normalized +-0.5, which is a binary
    fraction, so a delta is a product both sides hold exactly."""
    axes = axes_of(path)
    tags = {a[0] for a in axes}
    if not axes:
        return []
    if tags & {"wght", "wdth"}:
        return [l for l in LOCATIONS[1:]
                if all(part.split("=")[0] in tags for part in l.split(","))]

    def at(tag, lo, df, hi, frac):
        return "%s=%r" % (tag, df + frac * ((hi - df) if frac > 0 else (df - lo)))

    out = []
    for frac in (0.5, -0.5):
        for tag, lo, df, hi in axes:
            if (hi > df) if frac > 0 else (lo < df):
                out.append(at(tag, lo, df, hi, frac))
    for frac in (0.5, -0.5, 1.0, -1.0):
        out.append(",".join(at(tag, lo, df, hi, frac) for tag, lo, df, hi in axes
                            if ((hi > df) if frac > 0 else (lo < df))))
    return out


def has_color_tables(path):
    """Whether the sfnt directory names COLR or CPAL, read here, not by either side."""
    try:
        with open(path, "rb") as handle:
            head = handle.read(12)
            count = int.from_bytes(head[4:6], "big")
            tags = {handle.read(16)[:4] for _ in range(count)}
    except (OSError, ValueError):
        return False
    return b"COLR" in tags or b"CPAL" in tags


def first_diff(a, b, path=""):
    """Where two JSON values first differ: a path and the two values."""
    if type(a) != type(b):
        return path, a, b
    if isinstance(a, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                return path + "/" + str(key), a.get(key), b.get(key)
            found = first_diff(a[key], b[key], path + "/" + str(key))
            if found:
                return found
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return path + " (length)", len(a), len(b)
        for i, (x, y) in enumerate(zip(a, b)):
            found = first_diff(x, y, "%s[%d]" % (path, i))
            if found:
                return found
        return None
    return None if a == b else (path, a, b)


def main(argv):
    quiet = "--quiet" in argv
    rest = [a for a in argv[1:] if not a.startswith("--")]
    if rest:
        fonts = [os.path.abspath(a) for a in rest]
        real = 0
    else:
        corpus_fonts = [f for f in corpus.fonts("sfnt") if has_color_tables(f)]
        real = len(corpus_fonts)
        fixtures = [os.path.join(FIXTURES, n) for n in sorted(os.listdir(FIXTURES))
                    if n.endswith((".ttf", ".otf")) and has_color_tables(
                        os.path.join(FIXTURES, n))]
        fonts = fixtures + corpus_fonts
    os.makedirs(SCRATCH, exist_ok=True)
    failed = compared = 0
    glyphs = nodes = 0
    wanted = {font: locations_for(font) for font in fonts}
    every = [None] + sorted({l for ls in wanted.values() for l in ls})
    for number, location in enumerate(every):
        flag = ["--location", location] if location else []
        ours = {}
        coords = {}
        for font in fonts:
            if location and location not in wanted[font]:
                continue
            run = subprocess.run([DRIVER] + flag + [font], capture_output=True,
                                 text=True)
            try:
                got = json.loads(run.stdout)
            except ValueError:
                got = {"error": run.stderr.strip()[:200] or "no answer"}
            if "coords" in got:
                coords[font] = got.pop("coords")
            ours[font] = got
        out = os.path.join(SCRATCH, "fonttools-%d.json" % number)
        given = os.path.join(SCRATCH, "coords-%d.json" % number)
        with open(given, "w", encoding="utf-8") as handle:
            json.dump(coords, handle)
        argv_ref = oracle_env.command("fonttools", [
            "python3", os.path.join(ROOT, "tools", "oracle", "fonttools_color.py")]
            + flag + ["--coords", given, out] + list(ours), scratch=SCRATCH)
        finished = subprocess.run(argv_ref, capture_output=True, text=True)
        if finished.returncode != 0:
            raise oracle_env.OracleUnavailable(
                "fonttools_color failed: %s" % finished.stderr.strip()[-300:])
        with open(out, encoding="utf-8") as handle:
            want = json.load(handle)
        for font, got in ours.items():
            compared += 1
            ref = want[font]
            if not location:
                glyphs += len(ref.get("layers") or {})
                nodes += len(json.dumps(ref.get("paints") or {}).split('{"f"')) - 1
            if got != ref:
                failed += 1
                print("DIFF %s %s" % (os.path.relpath(font, ROOT), location or "default"))
                where = first_diff(got, ref)
                if where:
                    print("     %s: this library %s, fontTools %s" % where)
    print("fonts %d (%d from the image's corpus), %d comparisons over %d locations, "
          "%d version 0 glyphs, %d paint nodes, %d differing"
          % (len(fonts), real, compared, len(every), glyphs, nodes, failed))
    return 1 if failed or not fonts else 0


if __name__ == "__main__":
    try:
        oracle_env.provenance(["fonttools"])
        sys.exit(main(sys.argv))
    except oracle_env.OracleUnavailable as why:
        sys.stderr.write("SKIPPED color_diff: %s\n" % why)
        sys.exit(1 if os.environ.get("GHOTI_ORACLE_REQUIRED") == "1" else 0)
