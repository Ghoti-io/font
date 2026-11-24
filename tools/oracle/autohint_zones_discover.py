#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Find the blue zones FreeType's automatic hinter uses for a script, from its answers.

    autohint_zones_discover.py RECORDS.jsonl [--tol UNITS_PER_1000]

RECORDS is what `autohint_diff.py --script RANGE --records RECORDS FONT` writes: each glyph's outline in font units with
the hinter's answer at several sizes. A glyph's top (or bottom) is *snapped* when FreeType's answer is a whole pixel at
two or more sizes while the outline's own scaled extreme is not; the snapped extremes are grouped by their height in font
units, and each group of three or more glyphs is a zone, listed with the letters that are in it. Run it for a script
and the letters of each zone are the string that script's entry in src/hint/autofit.c names.
"""
import collections
import json
import sys


def main(argv):
    path = argv[1]
    tol = 12
    if "--tol" in argv:
        tol = int(argv[argv.index("--tol") + 1])
    by_glyph = collections.defaultdict(list)
    upem = 1000
    for line in open(path):
        r = json.loads(line)
        upem = r["upem"]
        by_glyph[r["ch"]].append(r)
    tops = []
    bottoms = []
    for ch, rs in by_glyph.items():
        ys = [p[1] for p in rs[0]["pts"]]
        if not ys:
            continue
        top_votes = bot_votes = 0
        for r in rs:
            s = r["ppem"] * 64.0 / r["upem"]
            theirs = [p[1] for p in r["theirs"]]
            if not theirs:
                continue
            tt, tb = max(theirs), min(theirs)
            if tt % 64 == 0 and abs(round(max(ys) * s) - tt) >= 2 and abs(max(ys) * s - round(max(ys) * s / 64) * 64) > 1:
                top_votes += 1
            if tb % 64 == 0 and abs(round(min(ys) * s) - tb) >= 2 and abs(min(ys) * s - round(min(ys) * s / 64) * 64) > 1:
                bot_votes += 1
        # Also the glyph already on a pixel before hinting is a zone member the votes cannot see: counted as a
        # vote only when the outline's extreme moved, so a run of zero-moved members is listed separately.
        if top_votes >= 2:
            tops.append((max(ys), ch))
        if bot_votes >= 2:
            bottoms.append((min(ys), ch))

    def group(items, label):
        items.sort()
        groups = []
        for y, ch in items:
            if groups and y - groups[-1][-1][0] <= tol * upem / 1000.0:
                groups[-1].append((y, ch))
            else:
                groups.append([(y, ch)])
        for g in groups:
            if len(g) < 3:
                continue
            ys = sorted(y for y, _ in g)
            print("%-6s y %5d..%5d median %5d  n=%3d  %s" % (label, ys[0], ys[-1], ys[len(ys) // 2], len(g),
                  "".join(ch for _, ch in g)))
    print("glyphs %d, upem %d" % (len(by_glyph), upem))
    group(tops, "top")
    group(bottoms, "bottom")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
