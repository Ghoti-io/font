#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""What FreeType's hinting does to CFF and Type 1 outlines, measured (plan C0).

    hint_cff_explore.py [--pool sfnt|type1] [--ppem N[,N...]] [--stride N] [--fonts N]

For each CFF-flavoured font (or Type 1 program) of the corpus, at each size, the outline of
every sampled glyph is loaded from FreeType unhinted (`version 0`, FT_LOAD_NO_HINTING) and
hinted (the `normal` and `light` targets), and the two are compared point by point in 26.6:
how many glyphs move at all, on which axis, by how much, whether the point count changes, and
whether the advance changes. Nothing here is this library's output: it measures the oracle,
to fix the scope of the engine before one is written. Only FreeType is run.

"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env


def parse(text):
    """{glyph: (advance, [(x, y)...], ncontours) | str}"""
    glyphs = {}
    current = None
    for line in text.splitlines():
        if line.startswith("g"):
            f = line.split()
            key = int(f[0][1:])
            if f[1] == "adv":
                current = (int(f[2]), [], [])
                glyphs[key] = current
            else:
                glyphs[key] = " ".join(f[1:])
                current = None
        elif current is not None and line.startswith("e"):
            current[2].extend(int(v) for v in line.split()[1:])
        elif current is not None and line.startswith("p "):
            f = line.split()
            current[1].append((int(f[1]), int(f[2])))
    return glyphs


def freetype(path, version, ppem, stride, target):
    argv = oracle_env.command("freetype", ["freetype-hinted", path, "0", str(version), str(ppem),
                                            str(stride), "0", target])
    done = subprocess.run(argv, capture_output=True, text=True)
    if done.returncode != 0:
        return None
    return parse(done.stdout)


def is_cff(path):
    with open(path, "rb") as f:
        head = f.read(12)
        if head[:4] != b"OTTO":
            return False
    return True


def main(argv):
    pool = "sfnt"
    ppems = [9, 11, 12, 13, 16, 17, 22, 32]
    stride = 5
    limit = None
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
    paths = corpus.fonts(pool)
    if pool == "sfnt":
        paths = [p for p in paths if is_cff(p)]
    if limit:
        paths = paths[:limit]
    if not paths:
        print("hint_cff_explore: no fonts in the %s pool (an empty measurement says nothing)" % pool)
        return 1
    print("%d fonts in the %s pool" % (len(paths), pool))
    for target in ("normal", "light"):
        print("\n== target %s ==" % target)
        print("%5s %8s %8s %8s %8s %8s %8s %8s %8s" % ("ppem", "glyphs", "moved", "x moved", "y moved",
              "max|dx|", "max|dy|", "pt diff", "adv diff"))
        for ppem in ppems:
            t = dict(glyphs=0, moved=0, xm=0, ym=0, dx=0, dy=0, shape=0, adv=0)
            worst = []
            for path in paths:
                plain = freetype(path, 0, ppem, stride, target)
                hinted = freetype(path, 35, ppem, stride, target)
                if plain is None or hinted is None:
                    continue
                for g, a in plain.items():
                    b = hinted.get(g)
                    if isinstance(a, str) or b is None or isinstance(b, str):
                        continue
                    t["glyphs"] += 1
                    if len(a[1]) != len(b[1]) or a[2] != b[2]:
                        t["shape"] += 1
                        continue
                    mx = max([abs(p[0] - q[0]) for p, q in zip(a[1], b[1])] or [0])
                    my = max([abs(p[1] - q[1]) for p, q in zip(a[1], b[1])] or [0])
                    if mx or my:
                        t["moved"] += 1
                    t["xm"] += mx != 0
                    t["ym"] += my != 0
                    t["dx"] = max(t["dx"], mx)
                    t["dy"] = max(t["dy"], my)
                    t["adv"] += a[0] != b[0]
                    if mx:
                        worst.append((mx, os.path.basename(path), g))
            print("%5d %8d %8d %8d %8d %8d %8d %8d %8d" % (ppem, t["glyphs"], t["moved"], t["xm"],
                  t["ym"], t["dx"], t["dy"], t["shape"], t["adv"]))
            for mx, name, g in sorted(worst, reverse=True)[:3]:
                print("        an x move of %d (26.6 units) in %s glyph %d" % (mx, name, g))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
