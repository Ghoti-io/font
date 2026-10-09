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
"""Vertical text in random variable fonts with `vmtx`, `VVAR` and `VORG`, both shapers.

A font per seed with an `fvar`, a `vhea` and `vmtx`, and in some a `VVAR` (advance
height, top and bottom bearing, and vertical origin mappings or none, over an item
variation store of random regions) and a `VORG`. Twelve strings per font shaped top
to bottom at a random location. Nothing is committed.

Usage: varvert_random_diff.py [--seeds N] [--first K] [-v] [--driver PATH]
"""

import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
import var_random_diff as V
from gsub_random_diff import u16, u32, base_font

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "varvert-random")


def s16(v):
    return u16(v & 0xFFFF)


def vertical_tables(seed, n=30):
    """A `vhea`, `vmtx` (and in some fonts a `VORG`) of random values, for other generators."""
    r = random.Random(seed * 3 + 11)
    nv = r.randint(1, n)
    vhea = (u32(0x10000) + s16(r.randint(300, 900)) + s16(-r.randint(200, 800)) + s16(0)
            + u16(1500) + s16(0) * 3 + s16(1) + s16(0) * 2 + s16(0) * 4 + s16(0) + u16(nv))
    vmtx = (b''.join(u16(r.randint(300, 1500)) + s16(r.randint(-100, 300)) for _ in range(nv))
            + b''.join(s16(r.randint(-100, 300)) for _ in range(n - nv)))
    tables = {'vhea': vhea, 'vmtx': vmtx}
    if r.random() < .4:
        ms = sorted(r.sample(range(1, n), r.randint(0, 3)))
        tables['VORG'] = u16(1, 0) + s16(r.randint(300, 1200)) + u16(len(ms)) + b''.join(
            u16(g) + s16(r.randint(-100, 1300)) for g in ms)
    return tables


def main(argv):
    seeds, first, driver, scratch = 200, 0, G.DRIVER, SCRATCH
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scratch": scratch = argv[i + 1]
    os.makedirs(scratch, exist_ok=True)
    cmap = {65 + i: i + 1 for i in range(8)}
    lines = []
    locs = {}
    n = 30
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        r = random.Random(seed * 3 + 5)
        naxes = r.choice((1, 1, 2))
        tags = ['wght', 'wdth'][:naxes]
        ranges = []
        for _ in tags:
            lo = r.choice((100, 200, 0, 50))
            hi = lo + r.choice((400, 600, 800))
            ranges.append((lo, lo + (hi - lo) // 2, hi))
        fvar = (u16(1, 0, 16, 2, naxes, 20, 0, 4 + 4 * naxes)
                + b''.join(t.encode() + V.fixed(lo) + V.fixed(d) + V.fixed(hi) + u16(0, 256 + k)
                           for k, (t, (lo, d, hi)) in enumerate(zip(tags, ranges))))
        nv = r.randint(1, n)
        vhea = (u32(0x10000) + s16(r.randint(300, 900)) + s16(-r.randint(200, 800)) + s16(0)
                + u16(1500) + s16(0) * 3 + s16(1) + s16(0) * 2 + s16(0) * 4 + s16(0) + u16(nv))
        assert len(vhea) == 36
        vmtx = (b''.join(u16(r.randint(300, 1500)) + s16(r.randint(-100, 300)) for _ in range(nv))
                + b''.join(s16(r.randint(-100, 300)) for _ in range(n - nv)))
        tables = {'fvar': fvar, 'vhea': vhea, 'vmtx': vmtx}
        vg = V.VarGen(seed * 5 + 1, naxes)
        store, counts = vg.store(n)
        # VVAR: store, then maps (0 = none) for advance, tsb, bsb, vorg.
        maps = [0, 0, 0, 0]
        body = b''
        head = 24
        if r.random() < .5:
            # An advance map: format 0, one byte entries, outer 0, inner = glyph (clamped)
            entries = bytes(min(g, counts[0] - 1) for g in range(n))
            m = u16(0x0000 | 0x0000) + u16(n) + entries   # format 0, entryFormat 0 => 1 byte, inner bits 4? set below
            # entryFormat: low 4 bits = inner bits - 1, bits 4-5 = entry size - 1.
            inner_bits = max(1, (counts[0] - 1).bit_length())
            m = u16(inner_bits - 1) + u16(n) + entries
            maps[0] = head + len(store)
            body += m
        if r.random() < .5:
            entries = bytes(min((g * 3) % n, counts[0] - 1) for g in range(n))
            inner_bits = max(1, (counts[0] - 1).bit_length())
            maps[3] = head + len(store) + len(body)
            body += u16(inner_bits - 1) + u16(n) + entries
        if r.random() < .5:
            # A top side bearing map, the same shape, over a different spread of inner indices.
            entries = bytes(min((g * 5 + 1) % n, counts[0] - 1) for g in range(n))
            inner_bits = max(1, (counts[0] - 1).bit_length())
            maps[1] = head + len(store) + len(body)
            body += u16(inner_bits - 1) + u16(n) + entries
        vvar = u16(1, 0) + u32(head) + u32(maps[0], maps[1], maps[2], maps[3]) + store + body
        if r.random() < .8:
            tables['VVAR'] = vvar
        if r.random() < .3:
            k = r.randint(0, 3)
            ms = sorted(r.sample(range(1, n), min(k, n - 1)))
            tables['VORG'] = u16(1, 0) + s16(r.randint(300, 1200)) + u16(len(ms)) + b''.join(
                u16(g) + s16(r.randint(-100, 1300)) for g in ms)
        if r.random() < .7:
            # Outlines: a rectangle per glyph, so a glyph has a box for the origin to hang from.
            glyf = b''
            offs = [0]
            for g in range(n):
                if g == 0 or g > 8:
                    offs.append(len(glyf))
                    continue
                x0, y0 = r.randint(0, 100), r.randint(-100, 100)
                x1, y1 = x0 + r.randint(100, 600), y0 + r.randint(100, 800)
                pts = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
                out = s16(1) + s16(x0) + s16(y0) + s16(x1) + s16(y1) + u16(3, 0) + bytes([1, 1, 1, 1])
                px = py = 0
                for (x, y) in pts:
                    out += s16(x - px)
                    px = x
                for (x, y) in pts:
                    out += s16(y - py)
                    py = y
                out += b'\0' * ((-len(out)) % 4)
                glyf += out
                offs.append(len(glyf))
            tables['glyf'] = glyf
            tables['maxp'] = u32(0x10000) + u16(n, 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)
            tables['loca'] = b''.join(u16(o // 2) for o in offs)
        with open(font, "wb") as h:
            h.write(base_font(nglyphs=n, cmap_map=cmap, extra=tables))
        rr = random.Random(seed * 7 + 1)
        loc = ','.join('%s=%d' % (t, rr.randint(lo - 20, hi + 20)) for t, (lo, d, hi) in zip(tags, ranges))
        locs[seed] = loc
        with open(text, "w") as h:
            h.write(''.join(''.join(chr(65 + rr.randrange(8)) for _ in range(rr.randint(1, 6))) + "\n"
                            for _ in range(12)))
        lines.append("hb-shape --font-file='%s' --output-format=json --variations='%s' --direction=ttb "
                     "--no-glyph-names --text-file='%s' > '%s.hb' 2>/dev/null" % (font, loc, text, font))
    runner = os.path.join(scratch, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=scratch),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
    keys = ("g", "cl", "ax", "ay", "dx", "dy")
    norm = lambda g: [tuple(x.get(k) for k in keys) for x in g]
    bad = compared = skipped = shown = 0
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        with open(font + ".hb", encoding="utf-8") as h:
            hb = [json.loads(l) if l.startswith("[") else [] for l in h.read().split("\n")[:-1]]
        with open(text, "rb") as h:
            ours = subprocess.run([driver, "--batch", "--script", "latn", "--ttb", "--location",
                                   locs[seed], font], stdin=h, capture_output=True, text=True, timeout=20)
        mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
        txt = open(text).read().split("\n")
        for i, (a, b) in enumerate(zip(hb, mine)):
            compared += 1
            if not isinstance(b, list) or norm(a) != norm(b):
                bad += 1
                if shown < 6 and "-v" in argv:
                    shown += 1
                    print("seed", seed, locs[seed], txt[i], "\n  hb ", norm(a)[:4],
                          "\n  our", b if not isinstance(b, list) else norm(b)[:4])
    print("varvert_random_diff: %d seeds from %d, %d strings, %d differ"
          % (seeds, first, compared, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
