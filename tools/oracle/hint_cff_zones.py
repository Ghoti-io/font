#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Where FreeType puts a blue-zone edge, as a table (plan C2, experiment 3).

    hint_cff_zones.py [--pool sfnt|type1] [--fonts N] [--stride N] [--ppem N[,N...]]

One tab-separated row per (font, size, stem edge inside a blue zone): the zone's bounds and
flat position in font units, the edge's own position, and the 26.6 height FreeType's hinted
outline gives the points that sit on it, with how many glyphs showed that height. The rule
that maps one to the other is fitted from this table offline; nothing here guesses it.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import hint_cff_explore as explore
import hint_cff_edges as edges


def main(argv):
    pool, limit, stride = "sfnt", None, 7
    ppems = [8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24, 28, 32]
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--pool":
            pool = args.pop(0)
        elif a == "--fonts":
            limit = int(args.pop(0))
        elif a == "--stride":
            stride = int(args.pop(0))
        elif a == "--ppem":
            ppems = [int(v) for v in args.pop(0).split(",")]
    paths = corpus.fonts(pool)
    if pool == "sfnt":
        paths = [p for p in paths if explore.is_cff(p)]
    if limit:
        paths = paths[:limit]
    print("font\tppem\tupem\tzone\tlo\thi\tflat\tedge\thinted\tglyphs\tblue_scale\tblue_shift\tblue_fuzz")
    for path in paths:
        data = edges.read_data(path, stride)
        if data is None:
            continue
        params, glyphs = data
        upem = params["upem"][0]
        zcache = {}
        for ppem in ppems:
            plain = explore.freetype(path, 0, ppem, stride, "normal")
            hinted = explore.freetype(path, 35, ppem, stride, "normal")
            if not plain or not hinted:
                continue
            scale = ppem * 64.0 / upem
            rows = collections.Counter()
            for gid, info in glyphs.items():
                if not info["valid"] or gid not in plain or gid not in hinted:
                    continue
                a, b = plain[gid], hinted[gid]
                if isinstance(a, str) or isinstance(b, str) or len(a[1]) != len(b[1]):
                    continue
                gp = info["params"]
                key = repr(sorted((k, tuple(v)) for k, v in gp.items()))
                if key not in zcache:
                    zcache[key] = (edges.zones(gp), gp.get("blue_fuzz", [65536])[0] / 65536.0)
                zs, fuzz = zcache[key]
                for kind, edge, width in info["stems"]:
                    if kind != "h" or width in (-20.0, -21.0):
                        continue
                    for e in (edge, edge + width):
                        z = edges.blue_of(e, zs, fuzz)
                        if not z:
                            continue
                        hit = [i for i, p in enumerate(a[1]) if abs(p[1] - e * scale) <= 1.0]
                        ys = sorted(set(b[1][i][1] for i in hit))
                        if len(ys) != 1:
                            continue
                        rows[(z, e, ys[0], gp.get("blue_scale", [0])[0], gp.get("blue_shift", [0])[0], gp.get("blue_fuzz", [0])[0])] += 1
            for (z, e, y, bsc, bsh, bfz), n in sorted(rows.items(), key=lambda kv: kv[0]):
                print("%s\t%d\t%d\t%s\t%g\t%g\t%g\t%g\t%d\t%d\t%d\t%d\t%d" % (
                    os.path.basename(path), ppem, upem, "bottom" if z[3] else "top", z[0], z[1], z[2], e, y, n,
                    bsc, bsh, bfz))
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
