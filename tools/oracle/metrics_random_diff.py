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
"""Random metrics tables, shaped here and by HarfBuzz in both directions.

A font per seed with a random `hhea` and `OS/2` (the typographic metrics, a Windows
box, the USE_TYPO_METRICS bit) and, in some, a `vhea` and `vmtx`, a `VORG`, an
`hmtx` with fewer metrics than glyphs; no layout tables. Random strings of its glyphs
shaped left to right and top to bottom, advances and offsets compared. Nothing is
committed.

Usage: metrics_random_diff.py [--seeds N] [--first K] [-v] [--driver PATH] [--scratch DIR]
"""

import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
from gsub_random_diff import u16, u32, sfnt

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "metrics-random")


def s16(v):
    return u16(v & 0xFFFF)


def font_for(seed):
    r = random.Random(seed)
    n = r.randint(6, 12)
    upem = r.choice((1000, 1000, 2048, 512, 1024))
    cmap = {65 + i: i + 1 for i in range(n - 1)}
    segs = sorted(cmap.items())
    sc = len(segs) + 1
    ends = u16(*[c for c, g in segs], 0xFFFF)
    starts = u16(*[c for c, g in segs], 0xFFFF)
    deltas = u16(*[(g - c) for c, g in segs], 1)
    f4 = u16(4, 0, 0, sc * 2, 0, 0, 0) + ends + u16(0) + starts + deltas + u16(*([0] * sc))
    f4 = u16(4, len(f4), 0) + f4[6:]
    cm = u16(0, 1, 3, 1) + u32(12) + f4
    asc, desc, gap = r.randint(200, 1500), -r.randint(0, 600), r.choice((0, 0, 90))
    nh = r.randint(1, n)
    hhea = u32(0x10000) + u16(asc, desc & 0xFFFF, gap, 1500, 0, 0, 1250, 1, 0, 0, 0, 0, 0, 0, 0, nh)
    hmtx = b''.join(u16(r.randint(100, 1200), 0) for _ in range(nh)) + b''.join(u16(0) for _ in range(n - nh))
    glyf = None
    head = (u32(0x10000, 0x10000, 0, 0x5F0F3CF5) + u16(0, upem) + b'\0' * 16
            + u16(0, 0, 1000, 1000, 0, 8, 2, 0, 0))
    maxp = u32(0x5000) + u16(n)
    t = {'head': head, 'hhea': hhea, 'maxp': maxp, 'hmtx': hmtx, 'cmap': cm}
    if r.random() < .7:
        sel = (r.getrandbits(1) << 7) | (r.getrandbits(1) << 6)
        os2 = (u16(4, 500, 400, 5, 0) + u16(*[0] * 10) + u16(0) * 2 + b'\0' * 10 + b'\0' * 16
               + b'VEND' + u16(sel, 32, 126) + s16(r.randint(300, 1400)) + s16(-r.randint(0, 500))
               + s16(r.choice((0, 0, 70))) + u16(r.randint(300, 1600), r.randint(0, 600)))
        os2 = os2 + u16(0, 0) + s16(0) * 0 + u16(0) * 0
        t['OS/2'] = os2 + b'\0' * 12
    if r.random() < .5:
        nv = r.randint(1, n)
        # 36 bytes: version, ascent, descent, line gap, max advance, three extents,
        # three caret fields, four reserved, the data format and the metric count.
        vhea = (u32(0x10000) + s16(r.randint(300, 900)) + s16(-r.randint(200, 800)) + s16(0)
                + u16(1500) + s16(0) * 3 + s16(1) + s16(0) * 2 + s16(0) * 4 + s16(0) + u16(nv))
        vmtx = b''.join(u16(r.randint(300, 1500)) + s16(r.randint(-100, 300)) for _ in range(nv)) \
            + b''.join(s16(r.randint(-100, 300)) for _ in range(n - nv))
        t['vhea'] = vhea
        t['vmtx'] = vmtx
    if r.random() < .5:
        k = r.randint(0, 3)
        ms = sorted(r.sample(range(1, n), min(k, n - 1)))
        t['VORG'] = u16(1, 0) + s16(r.randint(300, 1200)) + u16(len(ms)) + b''.join(
            u16(g) + s16(r.randint(-100, 1300)) for g in ms)
    return sfnt(t), n


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
        data, n = font_for(seed)
        with open(font, "wb") as h:
            h.write(data)
        rr = random.Random(seed * 7 + 1)
        with open(text, "w") as h:
            for _ in range(8):
                h.write(''.join(chr(65 + rr.randrange(n - 1)) for _ in range(rr.randint(1, 5))) + "\n")
        for d in ("ltr", "ttb"):
            lines.append("hb-shape --font-file='%s' --output-format=json --direction=%s "
                         "--no-glyph-names --text-file='%s' > '%s.%s.hb' 2>/dev/null"
                         % (font, d, text, font, d))
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
        for d, extra in (("ltr", []), ("ttb", ["--ttb"])):
            try:
                with open("%s.%s.hb" % (font, d), encoding="utf-8") as h:
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
                        print("seed", seed, d, txt[i], "\n  hb ", norm(a), "\n  our", norm(b))
    print("metrics_random_diff: %d seeds from %d, %d strings, %d differ, %d skipped"
          % (seeds, first, compared, bad, skipped))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
