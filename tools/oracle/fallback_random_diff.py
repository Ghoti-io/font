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
"""Marks placed with no GPOS, over random glyph boxes, here and in HarfBuzz.

A font per seed with a `glyf` table of one rectangle per glyph (random size and
place), a cmap of some letters and combining marks, and no layout tables at all, so
that a mark is positioned by the fallback that reads the glyphs' extents and the
marks' combining classes. Twelve random strings of letters with marks after them;
glyphs, clusters and positions compared. Nothing is committed.

Usage: fallback_random_diff.py [--seeds N] [--first K] [--set latn|hebr|arab|thai|deva]
                               [--direction rtl] [-v] [--driver PATH] [--scratch DIR]
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

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "fallback-random")

# (ISO tag, OT tag, bases, marks)
SETS = {
    "latn": ("Latn", "latn", [0x61, 0x62, 0x65, 0x6F, 0x69, 0x20],
             [0x300, 0x301, 0x302, 0x308, 0x323, 0x327, 0x328, 0x31B, 0x334, 0x345, 0x338, 0x35C,
              0x1AB0, 0x20D0]),
    "hebr": ("Hebr", "hebr", [0x5D0, 0x5D1, 0x5E9, 0x5D3],
             [0x5B0, 0x5B4, 0x5BC, 0x5C1, 0x5C2, 0x5B7, 0x5BF, 0x5BD, 0x5C4]),
    "arab": ("Arab", "arab", [0x627, 0x628, 0x62C, 0x644],
             [0x64B, 0x64E, 0x651, 0x653, 0x654, 0x655, 0x670, 0x6D6]),
    "thai": ("Thai", "thai", [0xE01, 0xE02, 0xE14, 0xE40],
             [0xE31, 0xE34, 0xE38, 0xE39, 0xE47, 0xE48, 0xE49, 0xE4D]),
    "deva": ("Deva", "dev2", [0x915, 0x916, 0x930],
             [0x93C, 0x941, 0x947, 0x94D, 0x901, 0x902, 0x951, 0x952]),
}


def box_font(chars, r):
    """A font whose glyph i+1 is a rectangle, with a cmap for chars."""
    n = len(chars) + 1
    glyphs, locs = b'', []
    for i in range(n):
        locs.append(len(glyphs))
        if i == 0:
            continue
        x0 = r.randint(-150, 400)
        y0 = r.randint(-300, 700)
        w, h = r.randint(40, 500), r.randint(40, 500)
        x1, y1 = x0 + w, y0 + h
        g = u16(1, x0, y0, x1, y1) + u16(3) + u16(0)
        g += bytes([1, 1, 1, 1])
        g += u16(x0, w, 0, -w) + u16(y0, 0, h, 0)
        g += b'\0' * (-len(g) % 4)
        glyphs += g
    locs.append(len(glyphs))
    loca = b''.join(u32(x) for x in locs)
    maxp = u32(0x10000) + u16(n, 4, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0)
    head = (u32(0x10000, 0x10000, 0, 0x5F0F3CF5) + u16(0, 1000) + b'\0' * 16
            + u16(0, 0, 1000, 1000, 0, 8, 2, 1, 0))
    cmap = {c: i + 1 for i, c in enumerate(chars)}
    return base_font(nglyphs=n, cmap_map=cmap,
                     extra={'glyf': glyphs, 'loca': loca, 'maxp': maxp, 'head': head})


def main(argv):
    seeds, first, driver, scratch, which = 200, 0, G.DRIVER, SCRATCH, "latn"
    direction = ""
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scratch": scratch = argv[i + 1]
        elif a == "--set": which = argv[i + 1]
        elif a == "--direction": direction = argv[i + 1]
    iso, ot, bases, marks = SETS[which]
    os.makedirs(scratch, exist_ok=True)
    lines = []
    for seed in range(first, first + seeds):
        r = random.Random(seed)
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        keep_marks = [m for m in marks if r.random() < .8] or marks[:2]
        chars = bases + keep_marks
        with open(font, "wb") as h:
            h.write(box_font(chars, r))
        rr = random.Random(seed * 7 + 1)
        with open(text, "w", encoding="utf-8") as h:
            for _ in range(12):
                s = ''
                for _ in range(rr.randint(1, 3)):
                    s += chr(rr.choice(bases))
                    s += ''.join(chr(rr.choice(marks)) for _ in range(rr.randint(0, 3)))
                h.write(s + "\n")
        opt = " --direction=%s" % direction if direction else ""
        lines.append("hb-shape --font-file='%s' --output-format=json%s "
                     "--no-glyph-names --script=%s --text-file='%s' > '%s.hb' 2>/dev/null"
                     % (font, opt, iso, text, font))
    runner = os.path.join(scratch, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=scratch),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
    extra = {"rtl": ["--rtl"], "": []}[direction]
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
                ours = subprocess.run([driver, "--batch", "--script", ot] + extra + [font],
                                      stdin=h, capture_output=True, text=True, timeout=20)
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
                if shown < 5 and "-v" in argv:
                    shown += 1
                    print("seed", seed, ' '.join('%X' % ord(ch) for ch in txt[i]),
                          "\n  hb ", norm(a), "\n  our", norm(b))
    print("fallback_random_diff: %s, %d seeds from %d, %d strings, %d differ, %d skipped"
          % (which, seeds, first, compared, bad, skipped))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
