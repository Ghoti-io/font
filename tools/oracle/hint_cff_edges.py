#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Where FreeType puts the edges of a CFF glyph's horizontal stems (plan C2, experiment 1).

    hint_cff_edges.py [--pool sfnt|type1] [--fonts N] [--stride N] [--ppem N[,N...]]

For each glyph with horizontal stems, the points of FreeType's unhinted outline that sit on a
stem edge are found, and the same points of its hinted outline are read: does an edge in a
blue zone land on the zone's flat position, rounded; does an edge outside any zone land on a
whole pixel; how wide is the stem afterwards against its scaled width. Only the *shape* of the
answer is wanted here (which hypotheses survive), so the output is counts and a few examples.
Analysis arithmetic is in Python floats; nothing here is the library's code.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import hint_cff_explore as explore

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DATA = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-hint-data")


def read_data(path, stride):
    run = subprocess.run([DATA, path, "0", str(stride)], capture_output=True, text=True)
    if run.returncode != 0:
        return None
    params, glyphs, cur = {}, {}, None
    first = None
    for line in run.stdout.splitlines():
        if line.startswith("P "):
            f = line.split()
            params[f[1]] = [int(v) for v in f[2:]]
        elif line.startswith("g"):
            f = line.split()
            gid = int(f[0][1:])
            cur = {"valid": f[1] == "valid", "stems": [], "masks": []} if f[1] in ("valid", "invalid") else None
            if cur:
                cur["params"] = {k: list(v) for k, v in params.items()}   # the Private DICT in force
                glyphs[gid] = cur
                if first is None:
                    first = cur["params"]
        elif line.startswith("s ") and cur is not None:
            f = line.split()
            cur["stems"].append((f[1], int(f[2]) / 65536.0, int(f[3]) / 65536.0))
        elif line.startswith("m ") and cur is not None:
            f = line.split()
            cur["masks"].append((int(f[1]), f[2]))
    return (first or params), glyphs


def zones(params):
    """[(lo, hi, flat, bottom)] in font units."""
    out = []
    bv = [v / 65536.0 for v in params.get("blue_values", [])]
    for i in range(0, len(bv), 2):
        lo, hi = bv[i], bv[i + 1]
        out.append((lo, hi, hi if i == 0 else lo, i == 0))   # the first pair is the baseline zone
    ob = [v / 65536.0 for v in params.get("other_blues", [])]
    for i in range(0, len(ob), 2):
        out.append((ob[i], ob[i + 1], ob[i + 1], True))
    return out


def blue_of(edge, zs, fuzz):
    for lo, hi, flat, bottom in zs:
        if lo - fuzz <= edge <= hi + fuzz:
            return (lo, hi, flat, bottom)
    return None


def dump(name, gid, ppem, pool):
    """One glyph's stems, the zones and what FreeType did, for reading by eye."""
    path = [p for p in corpus.fonts(pool) if os.path.basename(p) == name][0]
    params, glyphs = read_data(path, 1)
    upem = params["upem"][0]
    scale = ppem * 64.0 / upem
    plain = explore.freetype(path, 0, ppem, 1, "normal")[gid]
    hinted = explore.freetype(path, 35, ppem, 1, "normal")[gid]
    print("upem %d scale %.4f 26.6/unit; blue %s other %s blue_scale %s shift %s fuzz %s stdhw %s snaps %s" % (
        upem, scale, [v / 65536.0 for v in params.get("blue_values", [])],
        [v / 65536.0 for v in params.get("other_blues", [])], params.get("blue_scale"),
        params.get("blue_shift"), params.get("blue_fuzz"), params.get("std_hw"), params.get("stem_snap_h")))
    info = glyphs[gid]
    print("stems:", [(k, e, w) for k, e, w in info["stems"]], "masks:", info["masks"])
    print("%4s %10s %10s %9s" % ("pt", "unhinted y", "hinted y", "font y"))
    for i, (p, q) in enumerate(zip(plain[1], hinted[1])):
        print("%4d %10d %10d %9.1f" % (i, p[1], q[1], p[1] / scale))
    return 0


def main(argv):
    pool, limit, stride, ppems = "sfnt", None, 7, [12, 17]
    if argv[1:2] == ["--dump"]:
        return dump(argv[2], int(argv[3]), int(argv[4]), argv[5] if len(argv) > 5 else "sfnt")
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
    stats = {}
    examples = {}
    hist = {}
    oh = {}

    def count(key, ok, example):
        stats.setdefault(key, [0, 0])
        stats[key][0 if ok else 1] += 1
        if not ok and len(examples.setdefault(key, [])) < 6:
            examples[key].append(example)

    for path in paths:
        data = read_data(path, stride)
        if data is None:
            continue
        params, glyphs = data
        upem = params["upem"][0]
        zs = zones(params)
        fuzz = params.get("blue_fuzz", [65536])[0] / 65536.0
        for ppem in ppems:
            plain = explore.freetype(path, 0, ppem, stride, "normal")
            hinted = explore.freetype(path, 35, ppem, stride, "normal")
            if not plain or not hinted:
                continue
            scale = ppem * 64.0 / upem
            for gid, info in glyphs.items():
                if not info["valid"] or gid not in plain or gid not in hinted:
                    continue
                a, b = plain[gid], hinted[gid]
                if isinstance(a, str) or isinstance(b, str) or len(a[1]) != len(b[1]):
                    continue
                for kind, edge, width in info["stems"]:
                    if kind != "h" or width in (-20.0, -21.0):
                        continue
                    pair = []
                    for e in (edge, edge + width):
                        hit = [i for i, p in enumerate(a[1]) if abs(p[1] - e * scale) <= 1.0]
                        if not hit:
                            pair = None
                            break
                        pair.append((e, sorted(set(b[1][i][1] for i in hit))))
                    if not pair:
                        continue
                    for e, ys in pair:
                        z = blue_of(e, zs, fuzz)
                        unique = len(ys) == 1
                        key = "blue edge: all points on one hinted height" if z else "plain edge: all points on one hinted height"
                        count(key, unique, (os.path.basename(path), ppem, gid, e, ys))
                        if not unique:
                            continue
                        y = ys[0]
                        if z:
                            px = z[2] * scale / 64.0
                            off = (e - z[2]) * scale / 64.0
                            side = "flat" if abs(e - z[2]) < 0.5 else "overshoot"
                            if side == "flat":
                                bucket = int((px - int(px // 1)) * 10) if px >= 0 else int(((px % 1)) * 10)
                                hist.setdefault((ppem, bucket), [0, 0])
                                hist[(ppem, bucket)][0 if y // 64 > px // 1 else 1] += 1
                            else:
                                oh.setdefault((round(abs(off), 1) if abs(off) < 2 else 2.0, ppem), []).append(
                                    (round(px, 3), round(y / 64.0 - px, 3)))
                            want = round(z[2] * scale / 64.0) * 64
                            count("blue edge lands on round(flat*scale)", y == want,
                                  (os.path.basename(path), ppem, gid, e, "got", y, "flat", z[2], "want", want))
                        else:
                            count("plain edge lands on a whole pixel", y % 64 == 0,
                                  (os.path.basename(path), ppem, gid, e, "got", y, "scaled", e * scale))
                    (e0, y0s), (e1, y1s) = pair
                    if len(y0s) == 1 and len(y1s) == 1:
                        w = (y1s[0] - y0s[0]) / 64.0
                        want = (e1 - e0) * scale / 64.0
                        count("stem width within half a pixel of scaled", abs(w - want) <= 0.5,
                              (os.path.basename(path), ppem, gid, e0, "width", w, "scaled", want))
                        count("stem width is a whole number of pixels", (y1s[0] - y0s[0]) % 64 == 0,
                              (os.path.basename(path), ppem, gid, e0, "width", w, "scaled", want))
    for key in sorted(stats):
        yes, no = stats[key]
        print("%-52s %7d yes %7d no (%.1f%%)" % (key, yes, no, 100.0 * yes / max(1, yes + no)))
        for e in examples.get(key, []):
            print("        e.g. %s" % (e,))
    print("\nblue edge exactly on the flat position: frac of flat*scale in pixels (tenths) -> [rounded up, rounded down or kept]")
    for (ppem, bucket) in sorted(hist):
        print("  ppem %2d frac %.1f-%.1f: up %5d  not-up %5d" % (ppem, bucket / 10.0, (bucket + 1) / 10.0, hist[(ppem, bucket)][0], hist[(ppem, bucket)][1]))
    print("\nblue edge on the overshoot side: (overshoot px, ppem) -> samples of (flat px, hinted - flat px)")
    for key in sorted(oh):
        v = oh[key]
        print("  overshoot %.1f px, ppem %d: %d edges, first: %s" % (key[0], key[1], len(v), v[:4]))
    return 0 if stats else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
