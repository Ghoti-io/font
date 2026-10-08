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
"""GPOS anchors that name a contour point, in a variable font, at several locations.

A box glyph whose third point `gvar` lifts by up to 100 units, a mark, and a
`mark` lookup attaching the mark to an anchor of format 2 (a point of the base
glyph) whose stated coordinates are not where the point is. HarfBuzz follows the
point once the font is at a location; this compares the two at several.

Usage: anchor_point_diff.py [-v]
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
from gsub_random_diff import u16, u32, base_font

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "anchor-point")
DRIVER = G.DRIVER


def s16(v):
    return u16(v & 0xFFFF)


def layout(features, lookups):
    scripts = (u16(1) + b'latn' + u16(8) + u16(4) + u16(0) + u16(0) + u16(0xFFFF)
               + u16(len(features)) + b''.join(u16(i) for i in range(len(features))))
    fl = u16(len(features))
    at = 2 + 6 * len(features)
    ft = b''
    for tag, ls in features:
        fl += tag.encode() + u16(at + len(ft))
        ft += u16(0, len(ls), *ls)
    fl += ft
    ll = u16(len(lookups))
    bodies = b''
    base = 2 + 2 * len(lookups)
    for typ, flag, sub in lookups:
        ll += u16(base + len(bodies))
        bodies += u16(typ, flag, 1, 8) + sub
    ll += bodies
    return u16(1, 0, 10, 10 + len(scripts), 10 + len(scripts) + len(fl)) + scripts + fl + ll


def mark_base(anchor_format, stated, point):
    mark_cov = u16(1, 1, 2)
    base_cov = u16(1, 1, 1)
    mark_array = u16(1) + u16(0, 6) + u16(1) + s16(0) + s16(0)
    if anchor_format == 2:
        anchor = u16(2) + s16(stated[0]) + s16(stated[1]) + u16(point)
    else:
        anchor = u16(1) + s16(stated[0]) + s16(stated[1])
    base_array = u16(1) + u16(4) + anchor
    head = 12
    return (u16(1, head, head + len(mark_cov), 1, head + len(mark_cov) + len(base_cov),
                head + len(mark_cov) + len(base_cov) + len(mark_array))
            + mark_cov + base_cov + mark_array + base_array)


def font_bytes(anchor_format, stated, point, lift):
    n = 3
    box = [(0, 0), (300, 0), (300, 600), (0, 600)]
    g = s16(1) + s16(0) + s16(0) + s16(300) + s16(600) + u16(3, 0) + bytes([1] * 4)
    px = 0
    for x, _ in box:
        g += s16(x - px)
        px = x
    py = 0
    for _, y in box:
        g += s16(y - py)
        py = y
    g += b'\0' * (-len(g) % 4)
    mark = (s16(1) + s16(0) + s16(0) + s16(60) + s16(60) + u16(3, 0) + bytes([1] * 4)
            + s16(0) + s16(60) + s16(0) + s16(-60) + s16(0) + s16(0) + s16(60) + s16(0))
    mark += b'\0' * (-len(mark) % 4)
    glyf = g + mark
    loca = u16(0, 0, len(g) // 2, len(glyf) // 2)
    maxp = u32(0x10000) + u16(n, 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)
    fixed = lambda v: u32(v << 16)
    fvar = (u16(1, 0, 16, 2, 1, 20, 0, 8) + b'wght' + fixed(100) + fixed(400) + fixed(900)
            + u16(0, 256))
    # gvar: glyph 1 has one tuple, peak at the top of the axis, every point and the
    # four phantom points, the x deltas all 0 and the y delta of point 2 equal to lift.
    ys = [0, 0, lift, 0, 0, 0, 0, 0]
    ser = b'\0' + bytes([0x40 | 7]) + b''.join(s16(0) for _ in range(8)) \
        + bytes([0x40 | 7]) + b''.join(s16(v) for v in ys)
    tup = u16(len(ser), 0xA000, 0x4000)
    gd = u16(1, 4 + len(tup)) + tup + ser
    gd += b'\0' * (-len(gd) % 2)
    offs = u16(0, 0, len(gd) // 2, len(gd) // 2)
    gvar = u16(1, 0, 1, 0) + u32(0) + u16(n, 0) + u32(20 + len(offs)) + offs + gd
    gpos = layout([('mark', [0])], [(4, 0, mark_base(anchor_format, stated, point))])
    return base_font(nglyphs=n, cmap_map={65: 1, 0x301: 2},
                     extra={'glyf': glyf, 'loca': loca, 'maxp': maxp, 'fvar': fvar,
                            'gvar': gvar, 'GPOS': gpos})


def main(argv):
    os.makedirs(SCRATCH, exist_ok=True)
    cases = {"point2": (2, (50, 50), 2), "point0": (2, (50, 50), 0), "point9": (2, (50, 50), 9),
             "format1": (1, (50, 50), 0)}
    locs = ["", "wght=100", "wght=400", "wght=650", "wght=900"]
    text = os.path.join(SCRATCH, "t.txt")
    with open(text, "w", encoding="utf-8") as h:
        h.write("Á\n")
    lines = []
    for name, (fmt, stated, pt) in cases.items():
        with open(os.path.join(SCRATCH, name + ".ttf"), "wb") as h:
            h.write(font_bytes(fmt, stated, pt, 100))
        for i, loc in enumerate(locs):
            opt = " --variations='%s'" % loc if loc else ""
            lines.append("hb-shape --font-file='%s/%s.ttf' --output-format=json --no-glyph-names%s "
                         "--text-file='%s' > '%s/%s_%d.hb' 2>/dev/null" % (SCRATCH, name, opt, text, SCRATCH, name, i))
    with open(os.path.join(SCRATCH, "run.sh"), "w") as h:
        h.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", os.path.join(SCRATCH, "run.sh")],
                                            scratch=SCRATCH), capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
    keys = ("g", "cl", "ax", "ay", "dx", "dy")
    norm = lambda g: [tuple(x.get(k) for k in keys) for x in g]
    bad = total = 0
    for name in cases:
        for i, loc in enumerate(locs):
            with open("%s/%s_%d.hb" % (SCRATCH, name, i), encoding="utf-8") as h:
                hb = [json.loads(x) for x in h.read().split("\n")[:-1] if x.startswith("[")]
            args = ["--location", loc] if loc else []
            out = subprocess.run([DRIVER, "--batch", "--script", "latn"] + args +
                                 [os.path.join(SCRATCH, name + ".ttf")], stdin=open(text, "rb"),
                                 capture_output=True, text=True).stdout
            mine = [json.loads(x) for x in out.split("\n")[:-1]]
            total += 1
            if [norm(a) for a in hb] != [norm(b) for b in mine]:
                bad += 1
                if "-v" in argv:
                    print(name, loc or "default", "\n  hb ", [norm(a) for a in hb], "\n  our", [norm(b) for b in mine])
    print("anchor_point_diff: %d runs, %d differ" % (total, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
