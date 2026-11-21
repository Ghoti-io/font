#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Print one glyph's FreeType outline unhinted and hinted, side by side.

    hint_cff_points.py POOL FONT-BASENAME GLYPH PPEM

For finding out what a hinted CFF or Type 1 outline has that an unhinted one has not (a point
count that differs by one is the first case), and for reading what a hint did to each point.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import hint_cff_explore as explore


def one(path, version, ppem, glyph):
    out = explore.freetype(path, version, ppem, 1, "normal")
    return out.get(glyph) if out else None


def main(argv):
    pool, name, glyph, ppem = argv[1], argv[2], int(argv[3]), int(argv[4])
    path = [p for p in corpus.fonts(pool) if os.path.basename(p) == name][0]
    plain = one(path, 0, ppem, glyph)
    hinted = one(path, 35, ppem, glyph)
    for label, g in (("unhinted", plain), ("hinted", hinted)):
        print("%s: advance %d, %d points, contour ends %s" % (label, g[0], len(g[1]), g[2]))
    print("%4s  %-16s %-16s" % ("i", "unhinted", "hinted"))
    for i in range(max(len(plain[1]), len(hinted[1]))):
        a = "%d,%d" % plain[1][i] if i < len(plain[1]) else "-"
        b = "%d,%d" % hinted[1][i] if i < len(hinted[1]) else "-"
        print("%4d  %-16s %-16s" % (i, a, b))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
