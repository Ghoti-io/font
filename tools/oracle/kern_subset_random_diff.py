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
"""Random `kern` tables, subset here, and shaped by HarfBuzz before and after.

A font per seed with one to three random subtables (Apple's header on the even seeds and Microsoft's on the odd: formats 0, 2 and 3,
some malformed) over the glyphs A..H, subset to some of those characters so that the
glyph ids move, in both numbering modes. HarfBuzz shapes the original and the subset on
strings of the kept characters, and the advances and offsets must be the same: only the
subsetter's rewrite of the table is under test, with HarfBuzz as the judge of what the
table means.

Usage: kern_subset_random_diff.py [--seeds N] [--first K] [-v] [--keep BDFH]
"""

import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
import kerx_random_diff as K
from gsub_random_diff import base_font

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "kern-subset-random")
SUBSET = os.path.join(oracle_env.ROOT, "build", "linux", "release", "apps", "examples",
                      "font-subset")


def main(argv):
    seeds, first, keep = 300, 0, "BDFH"
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--keep": keep = argv[i + 1]
        elif a == "--subs": K.SUBS = int(argv[i + 1])
        elif a == "--kinds": K.AKINDS = tuple(int(x) for x in argv[i + 1].split(","))
    os.makedirs(SCRATCH, exist_ok=True)
    cmap = {65 + i: i + 1 for i in range(8)}
    unicodes = ",".join("%X" % ord(c) for c in keep)
    lines = []
    jobs = []
    for seed in range(first, first + seeds):
        font = os.path.join(SCRATCH, "s%d.ttf" % seed)
        text = os.path.join(SCRATCH, "s%d.txt" % seed)
        with open(font, "wb") as h:
            # Empty TrueType outlines (the subsetter takes only fonts that have some kind).
            n = 30
            h.write(base_font(nglyphs=n, cmap_map=cmap, extra={
                "kern": K.Gen(seed).kern_apple(seed % 2 == 0),
                "glyf": b"\0" * 4,
                "loca": b"\0" * (2 * (n + 1)),
                "maxp": G.u32(0x10000) + G.u16(n, *([0] * 13))}))
        rr = random.Random(seed * 7)
        with open(text, "w") as h:
            h.write("".join("".join(rr.choice(keep) for _ in range(rr.randint(1, 8))) + "\n"
                            for _ in range(12)))
        for mode in ("renumber", "retain"):
            out = os.path.join(SCRATCH, "s%d-%s.ttf" % (seed, mode))
            args = [SUBSET, "--unicodes", unicodes] + (["--retain-gids"] if mode == "retain" else [])
            run = subprocess.run(args + [font, out], capture_output=True, text=True)
            jobs.append((seed, mode, font, out, text, run.returncode, run.stderr.strip()))
        for path in (font, os.path.join(SCRATCH, "s%d-renumber.ttf" % seed),
                     os.path.join(SCRATCH, "s%d-retain.ttf" % seed)):
            lines.append("hb-shape --font-file='%s' --output-format=json --no-glyph-names "
                         "--text-file='%s' > '%s.hb' 2>/dev/null" % (path, text, path))
    runner = os.path.join(SCRATCH, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=SCRATCH),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2

    def load(path):
        with open(path + ".hb", encoding="utf-8") as h:
            return [json.loads(l) if l.startswith("[") else [] for l in h.read().split("\n")[:-1]]

    keys = ("ax", "ay", "dx", "dy")
    norm = lambda g: [tuple(x.get(k) for k in keys) for x in g]
    bad = compared = refused = 0
    shown = 0
    for seed, mode, font, out, text, code, err in jobs:
        if code != 0:
            refused += 1
            print("REFUSED seed %d %s: %s" % (seed, mode, err.splitlines()[0] if err else ""))
            continue
        want, got = load(font), load(out)
        txt = open(text).read().split("\n")
        for i, (a, b) in enumerate(zip(want, got)):
            compared += 1
            if norm(a) != norm(b):
                bad += 1
                if shown < 6 and "-v" in argv:
                    shown += 1
                    print("seed", seed, mode, txt[i], "\n  was", norm(a), "\n  now", norm(b))
    print("kern_subset_random_diff: %d seeds from %d, %d strings, %d differ, %d refused"
          % (seeds, first, compared, bad, refused))
    return 1 if (bad or refused) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
