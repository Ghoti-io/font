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
"""The box the shaper measures each glyph with, against HarfBuzz, in color fonts.

documentation/design.md section 14. `gfnt_glyph_extents()` is what places a mark
when no `GPOS` does, and HarfBuzz measures a glyph of a COLR version 1 font
differently from its outline: a color glyph by its clip box (or, with none, by
what its paint graph draws), and glyph 0 not at all. The shaping differential
could not see it until a COLR font was in the corpus, and then only through the
marks it moved; this holds the box itself, glyph by glyph, to `hb-shape
--show-extents` for every code point each font maps (and one it does not, which
is glyph 0).

Usage:
    color_extents_diff.py [--quiet] [-v] [font...]
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import color_diff
import strike_image_diff
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-extents")
SCRATCH = os.path.join(ROOT, "build", "oracle", "color-extents")

# Run in the fontTools image: each font's em, and the code points it maps.
DUMP = r"""
import json, sys
from fontTools.ttLib import TTFont
out = {}
for path in sys.argv[2:]:
    t = TTFont(path)
    # Controls are left out: a line of text cannot hold a line break, and a
    # control is not what a color font is for.
    cps = sorted(c for c in t.getBestCmap()
                 if c <= 0x10FFFF and c >= 0x20 and not 0x7F <= c <= 0x9F
                 and not 0xD800 <= c <= 0xDFFF and c not in (0x2028, 0x2029)
                 # HarfBuzz swaps a default-ignorable for a blank glyph before it
                 # measures anything, so these name a different glyph on each side.
                 and not (0x200B <= c <= 0x200F or 0x2060 <= c <= 0x206F
                          or 0xFE00 <= c <= 0xFE0F or 0xE0000 <= c <= 0xE0FFF
                          or c in (0xAD, 0x34F, 0x61C, 0x115F, 0x1160, 0x17B4, 0x17B5,
                                   0x3164, 0xFEFF, 0xFFA0)
                          or 0x180B <= c <= 0x180F or 0xFFF0 <= c <= 0xFFF8))
    free = next(c for c in (0xE000, 0xE001, 0x10FFFD) if c not in t.getBestCmap())
    sizes = []
    if "CBLC" in t:
        for strike in t["CBLC"].strikes:
            size = strike.bitmapSizeTable
            sizes.append(max(size.ppemX, size.ppemY))
    if "sbix" in t:
        sizes.extend(strike.ppem for strike in t["sbix"].strikes.values())
    out[path] = {"upem": t["head"].unitsPerEm, "cps": cps + [free],
                 "version": t["COLR"].version if "COLR" in t else None,
                 "sizes": sizes}
json.dump(out, open(sys.argv[1], "w"))
"""


def main(argv):
    quiet = "--quiet" in argv
    verbose = "-v" in argv
    rest = [a for a in argv[1:] if not a.startswith("-")]
    if rest:
        fonts = [os.path.abspath(a) for a in rest]
    else:
        fixtures = [os.path.join(color_diff.FIXTURES, n)
                    for n in sorted(os.listdir(color_diff.FIXTURES))
                    if n.endswith((".ttf", ".otf"))]
        fonts = [f for f in fixtures + corpus.fonts("sfnt")
                 if color_diff.has_color_tables(f)
                 or strike_image_diff.has_strike_tables(f)]
    os.makedirs(SCRATCH, exist_ok=True)
    script = os.path.join(SCRATCH, "dump.py")
    info = os.path.join(SCRATCH, "info.json")
    with open(script, "w", encoding="utf-8") as handle:
        handle.write(DUMP)
    ran = subprocess.run(oracle_env.command("fonttools",
        ["python3", script, info] + fonts, scratch=SCRATCH),
        capture_output=True, text=True)
    if ran.returncode != 0:
        raise oracle_env.OracleUnavailable("dump failed: %s" % ran.stderr[-300:])
    with open(info, encoding="utf-8") as handle:
        info = json.load(handle)

    lines = []
    for number, font in enumerate(fonts):
        with open(os.path.join(SCRATCH, "%d.txt" % number), "w", encoding="utf-8") as handle:
            handle.write("".join(chr(c) + "\n" for c in info[font]["cps"]))
        with open(os.path.join(SCRATCH, "%d.cps" % number), "w") as handle:
            handle.write("".join("%x\n" % c for c in info[font]["cps"]))
        lines.append("hb-shape --font-file='%s' --font-size=%d --show-extents "
                     "--no-glyph-names --output-format=json --text-file='%s/%d.txt' "
                     "> '%s/%d.hb' 2>/dev/null"
                     % (font, info[font]["upem"], SCRATCH, number, SCRATCH, number))
    with open(os.path.join(SCRATCH, "run.sh"), "w") as handle:
        handle.write("\n".join(lines) + "\n")
    ran = subprocess.run(oracle_env.command("harfbuzz",
        ["sh", os.path.join(SCRATCH, "run.sh")], scratch=SCRATCH),
        capture_output=True, text=True)
    if ran.returncode not in (0, 1):
        raise oracle_env.OracleUnavailable("hb-shape failed: %s" % ran.stderr[-300:])

    failed = compared = swept = 0
    by_version = {}

    def agrees(got, want):
        # `null` is this library saying it cannot measure the glyph, and an empty
        # box is how hb-shape prints the same refusal, so they are one answer.
        value = json.loads(got)
        return value == want or (value is None and want[1:] == [0, 0, 0, 0])

    for number, font in enumerate(fonts):
        cps = info[font]["cps"]
        with open(os.path.join(SCRATCH, "%d.cps" % number), "rb") as handle:
            mine = subprocess.run([DRIVER, font], stdin=handle, capture_output=True,
                                  text=True).stdout.split("\n")[:-1]
        with open(os.path.join(SCRATCH, "%d.hb" % number), encoding="utf-8") as handle:
            theirs = [json.loads(l) for l in handle.read().split("\n")[:-1]]
        by_version[info[font]["version"]] = by_version.get(info[font]["version"], 0) + 1
        if len(mine) != len(cps) or len(theirs) != len(cps):
            print("DIFF %s: %d code points, this library %d lines, HarfBuzz %d"
                  % (os.path.relpath(font, ROOT), len(cps), len(mine), len(theirs)))
            failed += 1
            continue
        shown = 0
        for cp, got, ref in zip(cps, mine, theirs):
            hb = ref[0]
            compared += 1
            want = [hb["g"], hb["xb"], hb["yb"], hb["w"], hb["h"]]
            if not agrees(got, want):
                failed += 1
                if shown < (50 if verbose else 1):
                    print("DIFF %s U+%04X: this library %s, HarfBuzz %s"
                          % (os.path.relpath(font, ROOT), cp, got, want))
                shown += 1

    # A color bitmap font is measured by the strike a size picks, and a shaper is
    # run with no size, which picks the largest; so the choice of strike is held
    # separately, by asking both for a spread of sizes round each strike's own.
    sweeps = []
    for number, font in enumerate(fonts):
        sizes = info[font]["sizes"]
        if not sizes or info[font]["version"] is not None:
            continue
        for ppem in sorted({1, 4000} | {n for s in sizes for n in (s - 1, s, s + 1)
                                        if n > 0}):
            sweeps.append((number, font, ppem))
    if sweeps:
        with open(os.path.join(SCRATCH, "sweep.sh"), "w") as handle:
            for number, font, ppem in sweeps:
                handle.write("hb-shape --font-file='%s' --font-size=%d --font-ppem=%d "
                             "--show-extents --no-glyph-names --output-format=json "
                             "--text-file='%s/%d.txt' > '%s/%d-%d.hbs' 2>/dev/null\n"
                             % (font, info[font]["upem"], ppem, SCRATCH, number,
                                SCRATCH, number, ppem))
        ran = subprocess.run(oracle_env.command("harfbuzz",
            ["sh", os.path.join(SCRATCH, "sweep.sh")], scratch=SCRATCH),
            capture_output=True, text=True)
        if ran.returncode not in (0, 1):
            raise oracle_env.OracleUnavailable("hb-shape failed: %s" % ran.stderr[-300:])
        for number, font, ppem in sweeps:
            cps = info[font]["cps"]
            with open(os.path.join(SCRATCH, "%d.cps" % number), "rb") as handle:
                mine = subprocess.run([DRIVER, "--ppem", str(ppem), font], stdin=handle,
                                      capture_output=True,
                                      text=True).stdout.split("\n")[:-1]
            with open(os.path.join(SCRATCH, "%d-%d.hbs" % (number, ppem)),
                      encoding="utf-8") as handle:
                theirs = [json.loads(l) for l in handle.read().split("\n")[:-1]]
            if len(mine) != len(cps) or len(theirs) != len(cps):
                print("DIFF %s at %d ppem: line counts differ" % (
                    os.path.relpath(font, ROOT), ppem))
                failed += 1
                continue
            for cp, got, ref in zip(cps, mine, theirs):
                hb = ref[0]
                swept += 1
                want = [hb["g"], hb["xb"], hb["yb"], hb["w"], hb["h"]]
                if not agrees(got, want):
                    failed += 1
                    if failed < 10:
                        print("DIFF %s U+%04X at %d ppem: this library %s, HarfBuzz %s"
                              % (os.path.relpath(font, ROOT), cp, ppem, got, want))
    print("color_extents_diff: %d fonts (COLR versions %s), %d glyph boxes, %d more "
          "across sizes of color bitmap strikes, %d differing"
          % (len(fonts), ", ".join("%s: %d" % kv for kv in sorted(
              by_version.items(), key=str)), compared, swept, failed))
    return 1 if failed or not fonts else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
