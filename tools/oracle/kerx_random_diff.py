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
"""Random AAT `kerx` (and legacy `kern`) tables, shaped here and by HarfBuzz.

A font per seed with one to three random subtables - ordered pairs (format 0) and
class arrays (format 2) in kerx, ordered pairs in kern - shaped with a dozen strings,
glyphs, advances and offsets compared. Nothing is committed: every font is a function
of its seed.

Usage: kerx_random_diff.py [--seeds N] [--first K] [--table kerx|kern] [-v]
                           [--direction rtl|ttb] [--driver PATH] [--scratch DIR]
"""

import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
from gsub_random_diff import u16, u32, base_font

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "kerx-random")
GL = list(range(1, 9))
KINDS = (0, 0, 2)
SUBS = 3
CROSS = 0.15


def s16(v):
    return u16(v & 0xFFFF)


def lookup8(m, first=1, last=14):
    return u16(8, first, last - first + 1, *[m.get(g, 0) for g in range(first, last + 1)])


def pad(b, n=4):
    return b + b'\0' * (-len(b) % n)


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)

    def pairs(self, k=None):
        r = self.r
        ps = {}
        for _ in range(k or r.randint(2, 14)):
            ps[(r.choice(GL), r.choice(GL))] = r.randint(-120, 120)
        return sorted(ps.items())

    def kerx0(self):
        ps = self.pairs()
        body = u32(len(ps), 0, 0, 0) + b''.join(u16(a, b) + s16(v) for (a, b), v in ps)
        return body

    def kerx2(self):
        r = self.r
        nleft, nright = r.randint(2, 4), r.randint(2, 4)
        row = 2 * nright
        # HarfBuzz reads the two class values as indices that add up to one.
        left = {g: r.randrange(nleft) * nright for g in GL if r.random() < .8}
        right = {g: r.randrange(nright) for g in GL if r.random() < .8}
        arr = b''.join(s16(r.randint(-120, 120)) for _ in range(nleft * nright))
        lt, rt = pad(lookup8(left, 1, 8)), pad(lookup8(right, 1, 8))
        # offsets are from the start of the subtable, whose header is 12 bytes
        base = 12 + 16
        return (u32(row, base, base + len(lt), base + len(lt) + len(rt)) + lt + rt + arr)

    def kerx(self):
        n = self.r.randint(1, SUBS)
        subs = b''
        for _ in range(n):
            kind = self.r.choice(KINDS)
            body = self.kerx0() if kind == 0 else self.kerx2()
            cov = kind | (0x40000000 if self.r.random() < CROSS else 0)
            body = pad(body)
            subs += u32(12 + len(body), cov, 0) + body
        return u16(2, 0) + u32(n) + subs

    def kern(self):
        n = self.r.randint(1, min(2, SUBS))
        subs = b''
        for _ in range(n):
            ps = self.pairs()
            cov = 0x0001 | (0x0004 if self.r.random() < .15 else 0) | (0x0002 if self.r.random() < .1 else 0)
            body = u16(len(ps), 0, 0, 0) + b''.join(u16(a, b) + s16(v) for (a, b), v in ps)
            subs += u16(0, 6 + len(body), cov) + body
        return u16(0, n) + subs


def main(argv):
    global KINDS, CROSS, SUBS
    seeds, first, driver, scratch, table = 200, 0, G.DRIVER, SCRATCH, "kerx"
    direction = ""
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scratch": scratch = argv[i + 1]
        elif a == "--table": table = argv[i + 1]
        elif a == "--direction": direction = argv[i + 1]
        elif a == "--kinds": KINDS = tuple(int(x) for x in argv[i + 1].split(","))
        elif a == "--subs": SUBS = int(argv[i + 1])
        elif a == "--cross": CROSS = float(argv[i + 1])
    os.makedirs(scratch, exist_ok=True)
    cmap = {65 + i: i + 1 for i in range(8)}
    lines = []
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        g = Gen(seed)
        with open(font, "wb") as h:
            h.write(base_font(nglyphs=30, cmap_map=cmap,
                              extra={table: g.kerx() if table == "kerx" else g.kern()}))
        rr = random.Random(seed * 7)
        with open(text, "w") as h:
            h.write(''.join(''.join(chr(65 + rr.randrange(8)) for _ in range(rr.randint(1, 8)))
                            + "\n" for _ in range(12)))
        opt = " --direction=%s" % direction if direction else ""
        lines.append("hb-shape --font-file='%s' --output-format=json%s "
                     "--no-glyph-names --text-file='%s' > '%s.hb' 2>/dev/null"
                     % (font, opt, text, font))
    runner = os.path.join(scratch, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=scratch),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
    extra = {"rtl": ["--rtl"], "ttb": ["--ttb"], "": []}[direction]
    bad = compared = skipped = 0
    keys = ("g", "cl", "ax", "ay", "dx", "dy")
    norm = lambda g: [tuple(x.get(k) for k in keys) for x in g]
    shown = 0
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        try:
            with open(font + ".hb", encoding="utf-8") as h:
                hb = [json.loads(l) if l.startswith("[") else [] for l in h.read().split("\n")[:-1]]
            with open(text, "rb") as h:
                ours = subprocess.run([driver, "--batch", "--script", "latn"] + extra + [font],
                                      stdin=h, capture_output=True, text=True, timeout=20)
            mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
        except (OSError, ValueError, subprocess.TimeoutExpired):
            skipped += 1
            continue
        if len(hb) != len(mine):
            skipped += 1
            continue
        txt = open(text).read().split("\n")
        for i, (a, b) in enumerate(zip(hb, mine)):
            compared += 1
            if norm(a) != norm(b):
                bad += 1
                if shown < 6 and "-v" in argv:
                    shown += 1
                    print("seed", seed, txt[i], "\n  hb ", norm(a), "\n  our", norm(b))
    print("kerx_random_diff: %s, %d seeds from %d, %d strings, %d differ, %d skipped"
          % (table, seeds, first, compared, bad, skipped))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
