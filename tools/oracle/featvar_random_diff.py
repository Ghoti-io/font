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
"""Random GSUB FeatureVariations, shaped here and by HarfBuzz at random locations.

A font per seed with an `fvar` of one or two axes and the random GSUB of
`gsub_random_diff.py` made version 1.1, with a FeatureVariations table of one to
three records. Each record has a condition set of up to two axis ranges (none
means always) and replaces one or two features by feature tables of other lookups.
The first record whose conditions hold at the location wins. Twelve strings per
font, shaped at a random location, compared glyph by glyph. Nothing is committed.

Usage: featvar_random_diff.py [--seeds N] [--first K] [-v] [--driver PATH]
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
from gsub_random_diff import u16, u32, base_font

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "featvar-random")


def s16(v):
    return u16(v & 0xFFFF)


def fixed(v):
    return u32(int(round(v * 65536)) & 0xFFFFFFFF)


def f2dot14(v):
    return s16(int(round(v * 16384)))


def feature_variations(r, nlookups, nfeats, naxes):
    records = []
    for _ in range(r.randint(1, 3)):
        conds = []
        for _ in range(r.choice((0, 1, 1, 2))):
            lo = r.choice((-1.0, -0.5, 0.0, 0.25, 0.5))
            hi = r.choice((0.25, 0.5, 1.0))
            if hi < lo:
                lo, hi = hi, lo
            conds.append(u16(1, r.randrange(naxes)) + f2dot14(lo) + f2dot14(hi))
        cs = u16(len(conds)) + b'' .join(u32(2 + 4 * len(conds) + sum(len(c) for c in conds[:k]))
                                         for k in range(len(conds))) + b''.join(conds)
        feats = sorted(r.sample(range(nfeats), min(nfeats, r.randint(1, 2))))
        alts = [u16(0, len(ls), *ls) for ls in
                (sorted(set(r.randrange(nlookups) for _ in range(r.randint(0, 2)))) for _ in feats)]
        head = 6 + 6 * len(feats)
        fts = u16(1, 0, len(feats))
        pos = head
        for fi, a in zip(feats, alts):
            fts += u16(fi) + u32(pos)
            pos += len(a)
        fts += b''.join(alts)
        records.append((cs, fts))
    head = 8 + 8 * len(records)
    out = u16(1, 0) + u32(len(records))
    pos = head
    bodies = b''
    for cs, fts in records:
        out += u32(pos) + u32(pos + len(cs))
        bodies += cs + fts
        pos += len(cs) + len(fts)
    return out + bodies


def main(argv):
    seeds, first, driver, scratch = 200, 0, G.DRIVER, SCRATCH
    direction = ""
    table = "gsub"
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scratch": scratch = argv[i + 1]
        elif a == "--direction": direction = argv[i + 1]
        elif a == "--table": table = argv[i + 1]
    os.makedirs(scratch, exist_ok=True)
    tags = G.TAGS_LATIN
    lines = []
    locs = {}
    for seed in range(first, first + seeds):
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        r = random.Random(seed * 11 + 2)
        naxes = r.choice((1, 1, 2))
        axtags = ['wght', 'wdth'][:naxes]
        ranges = []
        for _ in axtags:
            lo = r.choice((100, 200, 0, 50))
            hi = lo + r.choice((400, 600, 800))
            ranges.append((lo, lo + (hi - lo) // 2, hi))
        fvar = (u16(1, 0, 16, 2, naxes, 20, 0, 4 + 4 * naxes)
                + b''.join(t.encode() + fixed(lo) + fixed(d) + fixed(hi) + u16(0, 256 + k)
                           for k, (t, (lo, d, hi)) in enumerate(zip(axtags, ranges))))
        if table == "gpos":
            gsub = P.build_gpos(seed)
            ll = int.from_bytes(gsub[8:10], "big")
            nlookups = int.from_bytes(gsub[ll:ll + 2], "big")
            nfeats = int.from_bytes(gsub[int.from_bytes(gsub[6:8], "big"):][:2], "big")
        else:
            gsub = G.build_gsub(seed, G.GLYPHS, tags)
            lookups, feats = G.build_gsub.last
            nlookups, nfeats = len(lookups), len(feats)
        sl, fl, ll = (int.from_bytes(gsub[4 + 2 * k:6 + 2 * k], "big") for k in range(3))
        fv_off = len(gsub) + 4
        gsub = (u32(0x10001) + u16(sl + 4, fl + 4, ll + 4) + u32(fv_off) + gsub[10:]
                + feature_variations(r, nlookups, nfeats, naxes))
        with open(font, "wb") as h:
            h.write(base_font(nglyphs=30, cmap_map=G.LATIN, extra={'fvar': fvar, ('GPOS' if table == "gpos" else 'GSUB'): gsub,
                                                       **({'GDEF': P.gdef()} if table == "gpos" else {})}))
        rr = random.Random(seed * 7 + 1)
        loc = ','.join('%s=%d' % (t, rr.randint(lo - 20, hi + 20))
                       for t, (lo, d, hi) in zip(axtags, ranges))
        locs[seed] = loc
        with open(text, "w") as h:
            h.write(''.join(''.join(chr(65 + rr.randrange(8)) for _ in range(rr.randint(1, 7))) + "\n"
                            for _ in range(12)))
        lines.append("hb-shape --font-file='%s' --output-format=json --variations='%s' %s"
                     "--no-glyph-names --text-file='%s' > '%s.hb' 2>/dev/null"
                     % (font, loc, ("--direction=%s " % direction) if direction else "", text, font))
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
                ours = subprocess.run([driver, "--batch", "--script", "latn", "--location",
                                       locs[seed]] + ({"rtl": ["--rtl"], "ttb": ["--ttb"], "btt": ["--btt"], "": []}[direction]) + [font], stdin=h, capture_output=True,
                                      text=True, timeout=20)
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
                    print("seed", seed, locs[seed], txt[i], "\n  hb ", norm(a), "\n  our", norm(b))
    print("featvar_random_diff: %d seeds from %d, %d strings, %d differ, %d skipped"
          % (seeds, first, compared, bad, skipped))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
