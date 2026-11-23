#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Hinted CFF and Type 1 outlines, this library against FreeType, point by point (plan C4).

    hint_cff_diff.py [--pool sfnt|type1] [--ppem N[,N...]] [--stride N] [--fonts N] [--fonts-skip N] [--files PATH[,PATH...]] [--auto]

For each glyph of each CFF-flavoured font (or Type 1 program) at each size, the outline this
library hints from its stems and blue zones is compared with FreeType's, as integers in 26.6.
Reported: glyphs identical; glyphs whose every point is within 1/64 pixel; points identical;
advances identical; and the declined and refused counts, so that a population quietly dropped
cannot flatter the rest. Exits nonzero if the identical fraction falls under --floor.
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import hint_cff_explore as explore
import hint_diff

ROOT = hint_diff.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-hint-cff")


def ours(path, ppem, stride, coords="-", auto=False):
    done = subprocess.run([DRIVER, path, "0", str(ppem), str(stride), "0", coords] + (["auto"] if auto else []),
                          capture_output=True, text=True)
    if done.returncode != 0:
        return None
    return hint_diff.parse(done.stdout)


def main(argv):
    auto = False
    pool, ppems, stride, limit, floor, dump, coords, only, files = "sfnt", [9, 12, 17, 32], 11, None, 0.0, None, "-", None, None
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--pool":
            pool = args.pop(0)
        elif a == "--ppem":
            ppems = [int(v) for v in args.pop(0).split(",")]
        elif a == "--stride":
            stride = int(args.pop(0))
        elif a == "--fonts":
            limit = int(args.pop(0))
        elif a == "--floor":
            floor = float(args.pop(0))
        elif a == "--coords":
            coords = args.pop(0)
        elif a == "--auto":
            auto = True
        elif a == "--only":
            only = args.pop(0)
        elif a == "--files":
            files = [os.path.abspath(v) for v in args.pop(0).split(",")]
        elif a == "--dump":
            dump = open(args.pop(0), "w")
    paths = corpus.fonts(pool)
    if pool == "sfnt" and not auto:
        paths = [p for p in paths if explore.is_cff(p)]
    if files:
        paths = files
    if only:
        paths = [p for p in paths if os.path.basename(p) == only]
    if limit:
        paths = paths[:limit]
    t = dict(compared=0, same=0, within1=0, points=0, point_same=0, declined=0, refused=0, count_differs=0, adv=0)
    shown = 0
    for path in paths:
        for ppem in ppems:
            mine = ours(path, ppem, stride, coords, auto)
            theirs, _ = hint_diff.theirs(path, 35, ppem, stride, coords, "autolight" if auto else "normal")
            if mine is None or theirs is None:
                continue
            for glyph, answer in sorted(mine.items()):
                if isinstance(answer, str):
                    t["declined"] += 1
                    continue
                other = theirs.get(glyph)
                if other is None or isinstance(other, str):
                    t["refused"] += 1
                    continue
                t["compared"] += 1
                if answer[0] != other[0]:
                    t["adv"] += 1
                if dump and (len(answer[1]) != len(other[1]) or answer[1] != other[1]):
                    dump.write(json.dumps({"f": os.path.basename(path), "p": ppem, "g": glyph, "ours": answer[1], "theirs": other[1],
                                          "eo": answer[2], "et": other[2]}, separators=(",", ":")) + "\n")
                if len(answer[1]) != len(other[1]):
                    t["count_differs"] += 1
                    if shown < 6:
                        shown += 1
                        print("%s ppem %d glyph %d: %d points vs %d" % (os.path.basename(path), ppem, glyph,
                                                                      len(answer[1]), len(other[1])))
                    continue
                diffs = [max(abs(a[0] - b[0]), abs(a[1] - b[1])) for a, b in zip(answer[1], other[1])]
                t["points"] += len(diffs)
                t["point_same"] += sum(1 for d in diffs if d == 0)
                if all(d == 0 for d in diffs):
                    t["same"] += 1
                if all(d <= 1 for d in diffs):
                    t["within1"] += 1
                elif shown < 12:
                    shown += 1
                    where = next(i for i, d in enumerate(diffs) if d > 1)
                    print("%s ppem %d glyph %d: point %d ours %s theirs %s" % (
                        os.path.basename(path), ppem, glyph, where, answer[1][where], other[1][where]))
    n = max(1, t["compared"])
    print("compared %(compared)d glyphs: %(same)d identical, %(within1)d within 1/64; points %(point_same)d of %(points)d identical; "
          "%(count_differs)d with a different point count; %(adv)d advances differ; %(declined)d declined by this library, "
          "%(refused)d refused by FreeType" % t)
    return 0 if t["same"] / n >= floor else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
