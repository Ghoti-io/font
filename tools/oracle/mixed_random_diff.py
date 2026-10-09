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
"""Fonts with a random subset of every table, shaped here and by HarfBuzz.

The other random generators each build one kind of table. The rules that choose
between tables (which of kerx, kern and GPOS applies, whether morx or GSUB runs,
what marks do without a GPOS, how a missing metric table falls back) only show when
tables meet, so this one draws, per seed, a subset of: outlines (`glyf`), an `fvar`
with no `gvar`, `GDEF`, `GSUB`, `GPOS`, `kern`, `kerx` (with `ankr`), `morx` or
`mort`, and `vhea`/`vmtx`/`VORG`; a direction; and a location when there is an `fvar`.
Twelve strings per font; glyphs, clusters and positions are compared.

Usage: mixed_random_diff.py [--seeds N] [--first K] [--only a,b] [--drop a,b]
                            [--direction ltr|rtl|ttb|btt] [-v] [--driver PATH]
Tables: glyf fvar gdef gsub gpos kern kerx morx mort vert
"""

import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
import gpos_random_diff as P
import kerx_random_diff as K
import morx_random_diff as M
import varvert_random_diff as VV
from gsub_random_diff import u16, u32, base_font

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "mixed-random")
NG = 30
TYPES = (0, 1, 2, 4, 5)
ALL = ("glyf", "fvar", "gdef", "gsub", "gpos", "kern", "kerx", "morx", "mort", "vert")
# How likely each table is to be present.
ODDS = {"glyf": .5, "fvar": .25, "gdef": .5, "gsub": .5, "gpos": .5, "kern": .2,
        "kerx": .25, "morx": .2, "mort": .1, "vert": .3}


def glyf_tables(r):
    glyphs, locs = b'', []
    for i in range(NG):
        locs.append(len(glyphs))
        if i == 0 or r.random() < .1:
            continue
        x0, y0 = r.randint(-150, 400), r.randint(-300, 700)
        w, h = r.randint(40, 500), r.randint(40, 500)
        g = u16(1, x0, y0, x0 + w, y0 + h) + u16(3) + u16(0) + bytes([1, 1, 1, 1])
        g += u16(x0, w, 0, -w) + u16(y0, 0, h, 0)
        glyphs += g + b'\0' * (-len(g) % 4)
    locs.append(len(glyphs))
    maxp = u32(0x10000) + u16(NG, 4, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0)
    head = (u32(0x10000, 0x10000, 0, 0x5F0F3CF5) + u16(0, 1000) + b'\0' * 16
            + u16(0, 0, 1000, 1000, 0, 8, 2, 1, 0))
    return {'glyf': glyphs, 'loca': b''.join(u32(x) for x in locs), 'maxp': maxp, 'head': head}


def fvar_table():
    fx = lambda v: u32(v << 16)
    return u16(1, 0, 16, 2, 1, 20, 0, 8) + b'wght' + fx(100) + fx(400) + fx(900) + u16(0, 256)


def pick(seed, only, drop):
    r = random.Random(seed * 13 + 5)
    have = {t for t in ALL if r.random() < ODDS[t]}
    if "morx" in have:
        have.discard("mort")
    if only:
        have = set(only)
    return have - set(drop)


def build(seed, have):
    r = random.Random(seed * 17 + 1)
    extra = {}
    if "glyf" in have:
        extra.update(glyf_tables(r))
    if "fvar" in have:
        extra['fvar'] = fvar_table()
    if "gdef" in have:
        extra['GDEF'] = P.gdef()
    if "gsub" in have:
        extra['GSUB'] = G.build_gsub(seed, G.GLYPHS, G.TAGS_LATIN)
    if "gpos" in have:
        extra['GPOS'] = P.build_gpos(seed)
    if "kern" in have or "kerx" in have:
        g = K.Gen(seed)
        if "kern" in have:
            extra['kern'] = g.kern()
        if "kerx" in have:
            g = K.Gen(seed + 1)
            extra['kerx'] = g.kerx()
            if g.has_ankr:
                extra['ankr'] = g.ankr()
    if "morx" in have:
        extra['morx'] = M.Gen(seed).morx(TYPES, False)
    if "mort" in have:
        extra['mort'] = M.Gen(seed).mort(TYPES)
    if "vert" in have:
        extra.update(VV.vertical_tables(seed, NG))
    return base_font(nglyphs=NG, cmap_map={65 + i: i + 1 for i in range(8)}, extra=extra)


def main(argv):
    global TYPES
    seeds, first, driver, scratch = 300, 0, G.DRIVER, SCRATCH
    only, drop, direction = (), (), None
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scratch": scratch = argv[i + 1]
        elif a == "--only": only = tuple(argv[i + 1].split(","))
        elif a == "--drop": drop = tuple(argv[i + 1].split(","))
        elif a == "--direction": direction = argv[i + 1]
        elif a == "--kinds": K.KINDS = tuple(int(x) for x in argv[i + 1].split(","))
        elif a == "--cross": K.CROSS = float(argv[i + 1])
        elif a == "--subs": K.SUBS = int(argv[i + 1])
        elif a == "--types": TYPES = tuple(int(x) for x in argv[i + 1].split(","))
    os.makedirs(scratch, exist_ok=True)
    meta, lines = {}, []
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        have = pick(seed, only, drop)
        d = direction or random.Random(seed * 3 + 7).choice(("ltr", "ltr", "rtl", "ttb", "btt"))
        rr = random.Random(seed * 7 + 2)
        loc = "wght=%d" % rr.randint(80, 920) if "fvar" in have and rr.random() < .6 else ""
        meta[seed] = (have, d, loc)
        with open(font, "wb") as h:
            h.write(build(seed, have))
        with open(text, "w") as h:
            h.write(''.join(''.join(chr(65 + rr.randrange(8)) for _ in range(rr.randint(1, 8)))
                            + "\n" for _ in range(12)))
        lines.append("hb-shape --font-file='%s' --output-format=json --direction=%s %s"
                     "--no-glyph-names --script=Latn --text-file='%s' > '%s.hb' 2>/dev/null"
                     % (font, d, ("--variations='%s' " % loc) if loc else "", text, font))
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
    bad = compared = skipped = 0
    badseeds, shown = [], 0
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        have, d, loc = meta[seed]
        try:
            with open(font + ".hb", encoding="utf-8") as h:
                hb = [json.loads(l) if l.startswith("[") else [] for l in h.read().split("\n")[:-1]]
            flags = {"rtl": ["--rtl"], "ttb": ["--ttb"], "btt": ["--btt"], "ltr": []}[d]
            if loc:
                flags += ["--location", loc]
            with open(text, "rb") as h:
                ours = subprocess.run([driver, "--batch", "--script", "latn"] + flags + [font],
                                      stdin=h, capture_output=True, text=True, timeout=20)
            mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
        except (OSError, ValueError, subprocess.TimeoutExpired):
            skipped += 1
            continue
        if len(hb) != len(mine):
            skipped += 1
            continue
        txt = open(text).read().split("\n")
        seed_bad = False
        for i, (a, b) in enumerate(zip(hb, mine)):
            compared += 1
            if norm(a) != norm(b):
                bad += 1
                seed_bad = True
                if shown < 6 and "-v" in argv:
                    shown += 1
                    print("seed", seed, d, loc, sorted(have), txt[i], "\n  hb ", norm(a), "\n  our", norm(b))
        if seed_bad:
            badseeds.append(seed)
    print("mixed_random_diff: %d seeds from %d, %d strings, %d differ, %d skipped"
          % (seeds, first, compared, bad, skipped))
    if badseeds and "-s" in argv:
        print("bad seeds:", ' '.join("%d[%s,%s]" % (s, meta[s][1], '+'.join(sorted(meta[s][0]))) for s in badseeds[:40]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
