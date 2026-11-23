#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""What FreeType's automatic hinter does to an outline, measured (autohinter plan, experiment 1).

    autohint_explore.py [--font NAME] [--ppem N[,N...]] [--stride N] [--target autolight|auto]

For each sampled glyph at each size the outline FreeType loads unhinted (`version 0`) is compared with
the one it loads under the automatic hinter, point by point in 26.6: how many glyphs move, on which axis,
whether the point count changes, and how far points move. Only FreeType is run; nothing here is this
library's output. It fixes the scope of the engine before one is written.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import hint_cff_explore as explore


def main(argv):
    ppems, stride, target, name = [9, 12, 17, 32], 7, "autolight", None
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--ppem":
            ppems = [int(v) for v in args.pop(0).split(",")]
        elif a == "--stride":
            stride = int(args.pop(0))
        elif a == "--target":
            target = args.pop(0)
        elif a == "--font":
            name = args.pop(0)
    paths = [p for p in corpus.fonts("sfnt") if (name is None or os.path.basename(p) == name)]
    for path in paths:
        for ppem in ppems:
            plain = explore.freetype(path, 0, ppem, stride, "normal")
            auto = explore.freetype(path, 35, ppem, stride, target)
            if not plain or not auto:
                continue
            c = collections.Counter()
            dy = collections.Counter()
            for g, a in plain.items():
                b = auto.get(g)
                if isinstance(a, str) or b is None or isinstance(b, str):
                    c["refused"] += 1
                    continue
                if len(a[1]) != len(b[1]):
                    c["count differs"] += 1
                    continue
                if a[1] == b[1]:
                    c["unchanged"] += 1
                    continue
                xs = any(p[0] != q[0] for p, q in zip(a[1], b[1]))
                ys = any(p[1] != q[1] for p, q in zip(a[1], b[1]))
                c["x and y" if xs and ys else "x only" if xs else "y only"] += 1
                if a[0] != b[0]:
                    c["advance differs"] += 1
                for p, q in zip(a[1], b[1]):
                    dy[round((q[1] - p[1]) / 64.0)] += 1
            print("%s ppem %d: %s" % (os.path.basename(path), ppem, dict(c)))
            print("   y moves (px, count):", sorted(dy.items())[:9])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
