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
"""Variation sequences in random cmaps, shaped here and by HarfBuzz.

A font per seed with a cmap of a few ideographs (format 4 and, in some fonts, format
12 too) and a format 14 subtable of random default and non-default variation
sequences, and no layout tables. Twelve random strings of those ideographs, some
followed by a variation selector (also stray ones, and two in a row); glyphs, clusters
and advances compared. Nothing is committed.

Usage: uvs_random_diff.py [--seeds N] [--first K] [-v] [--driver PATH] [--scratch DIR]
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

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "uvs-random")
BASES = [0x4E00, 0x4E01, 0x4E02, 0x4E03, 0x4E04, 0x4E05, 0x20000, 0x20001, 0x3042, 0x31]
SELECTORS = [0xFE00, 0xFE01, 0xFE02, 0xFE0E, 0xFE0F, 0xE0100, 0xE0101, 0xE0102, 0xE01EF,
             0x180B, 0x180D]


def u24(v):
    return bytes([(v >> 16) & 255, (v >> 8) & 255, v & 255])


def cmap_table(seed):
    r = random.Random(seed)
    keep = [b for b in BASES if r.random() < .75] or BASES[:3]
    glyph = {b: i + 1 for i, b in enumerate(keep)}
    n = len(keep) + 6
    bmp = sorted((c, g) for c, g in glyph.items() if c < 0x10000)
    sc = len(bmp) + 1
    ends = u16(*[c for c, g in bmp], 0xFFFF)
    starts = u16(*[c for c, g in bmp], 0xFFFF)
    deltas = u16(*[(g - c) for c, g in bmp], 1)
    ro = u16(*([0] * sc))
    f4 = u16(4, 0, 0, sc * 2, 0, 0, 0) + ends + u16(0) + starts + deltas + ro
    f4 = u16(4, len(f4), 0) + f4[6:]
    with12 = any(c >= 0x10000 for c in glyph) or r.random() < .4
    groups = sorted(glyph.items())
    f12 = u16(12, 0) + u32(16 + 12 * len(groups), 0, len(groups)) + b''.join(
        u32(c, c, g) for c, g in groups)
    # format 14
    records = []
    for vs in sorted(r.sample(SELECTORS, r.randint(1, 5))):
        dflt = sorted(b for b in glyph if r.random() < .5)
        nondef = sorted((b, r.randint(1, n)) for b in glyph if r.random() < .5 and b not in dflt)
        records.append((vs, dflt, nondef))
    head_len = 10 + 11 * len(records)
    tables = b''
    recs = b''
    for vs, dflt, nondef in records:
        do = no = 0
        if dflt:
            do = head_len + len(tables)
            tables += u32(len(dflt)) + b''.join(u24(b) + bytes([0]) for b in dflt)
        if nondef:
            no = head_len + len(tables)
            tables += u32(len(nondef)) + b''.join(u24(b) + u16(g) for b, g in nondef)
        recs += u24(vs) + u32(do, no)
    f14 = u16(14) + u32(head_len + len(tables), len(records)) + recs + tables
    subs = [((0, 5), f14), ((3, 1), f4)]
    if with12:
        subs.append(((3, 10), f12))
    subs.sort(key=lambda s: s[0])
    off = 4 + 8 * len(subs)
    hdr, data = u16(0, len(subs)), b''
    for (pid, eid), t in subs:
        hdr += u16(pid, eid) + u32(off + len(data))
        data += t
    return hdr + data, n, glyph


def main(argv):
    seeds, first, driver, scratch = 200, 0, G.DRIVER, SCRATCH
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scratch": scratch = argv[i + 1]
    os.makedirs(scratch, exist_ok=True)
    lines = []
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        cm, n, glyph = cmap_table(seed)
        with open(font, "wb") as h:
            h.write(base_font(nglyphs=n, extra={'cmap': cm}))
        rr = random.Random(seed * 7 + 1)
        with open(text, "w", encoding="utf-8") as h:
            for _ in range(12):
                s = ''
                for _ in range(rr.randint(1, 4)):
                    s += chr(rr.choice(BASES))
                    for _ in range(rr.choice((0, 0, 1, 1, 2))):
                        s += chr(rr.choice(SELECTORS))
                h.write(s + "\n")
        lines.append("hb-shape --font-file='%s' --output-format=json "
                     "--no-glyph-names --text-file='%s' > '%s.hb' 2>/dev/null"
                     % (font, text, font))
    runner = os.path.join(scratch, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=scratch),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
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
                ours = subprocess.run([driver, "--batch", "--script", "latn", font], stdin=h,
                                      capture_output=True, text=True, timeout=20)
            mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
        except (OSError, ValueError, subprocess.TimeoutExpired):
            skipped += 1
            continue
        if len(hb) != len(mine):
            skipped += 1
            continue
        txt = open(text, encoding="utf-8").read().split("\n")
        for i, (a, b) in enumerate(zip(hb, mine)):
            compared += 1
            if norm(a) != norm(b):
                bad += 1
                if shown < 6 and "-v" in argv:
                    shown += 1
                    print("seed", seed, ' '.join('%X' % ord(ch) for ch in txt[i]),
                          "\n  hb ", norm(a), "\n  our", norm(b))
    print("uvs_random_diff: %d seeds from %d, %d strings, %d differ, %d skipped"
          % (seeds, first, compared, bad, skipped))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
