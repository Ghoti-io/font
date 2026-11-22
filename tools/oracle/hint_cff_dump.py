#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""A glyph-level data set for fitting the CFF hinting rules offline (plan C2).

    hint_cff_dump.py [--pool sfnt|type1] [--fonts N] [--stride N] [--ppem N[,N...]]

One JSON object per line: the font, the size, a glyph's horizontal stems and hint masks as this
library reads them, the Private DICT's hint parameters, the contour ends, and the y of every
point of FreeType's unhinted and hinted outlines (26.6). Only glyphs whose two outlines have
the same number of points are written. Nothing is decided here; a fitting script reads it.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import hint_cff_explore as explore
import hint_cff_edges as edges


def main(argv):
    pool, limit, stride = "sfnt", None, 13
    ppems = [9, 12, 17, 32, 48]
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
    for path in paths:
        data = edges.read_data(path, stride)
        if data is None:
            continue
        first, glyphs = data
        upem = first["upem"][0]
        for ppem in ppems:
            plain = explore.freetype(path, 0, ppem, stride, "normal")
            hinted = explore.freetype(path, 35, ppem, stride, "normal")
            if not plain or not hinted:
                continue
            for gid, info in glyphs.items():
                if not info["valid"] or gid not in plain or gid not in hinted:
                    continue
                a, b = plain[gid], hinted[gid]
                if isinstance(a, str) or isinstance(b, str) or len(a[1]) != len(b[1]):
                    continue
                gp = info["params"]
                # This library keeps a contour's closing point; the unhinted FreeType outline drops it when it equals the first
                # in font units. fy is this library's y of the points FreeType has, fyi their indexes in this library's outline
                # (which is what a hint mask's first point counts), or None when the two cannot be matched.
                fy_aligned, fyi = None, None
                if info.get("fy") and info.get("fx") and info.get("fc"):
                    keep = []
                    c = info["fc"]
                    for k in range(0, len(c), 2):
                        first, n = c[k], c[k + 1]
                        idx = list(range(first, first + n))
                        if n >= 2 and info["fy"][idx[-1]] == info["fy"][idx[0]] and info["fx"][idx[-1]] == info["fx"][idx[0]]:
                            idx = idx[:-1]
                        keep.extend(idx)
                    if len(keep) == len(a[1]):
                        fy_aligned, fyi = [info["fy"][i] for i in keep], keep
                keep = ("blue_values", "other_blues", "family_blues", "family_other_blues", "blue_scale",
                        "blue_shift", "blue_fuzz", "std_hw", "stem_snap_h")
                print(json.dumps({
                    "f": os.path.basename(path), "p": ppem, "u": upem, "g": gid,
                    "par": {k: gp.get(k, []) for k in keep},
                    "st": [[k, e, w] for k, e, w in info["stems"]],
                    "mk": info["masks"],
                    "adv": [a[0], b[0]],
                    "e": a[2],
                    "y0": [pt[1] for pt in a[1]], "y1": [pt[1] for pt in b[1]],
                    "x0": [pt[0] for pt in a[1]],
                    "fy": fy_aligned,
                    "fyi": fyi,
                }, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
