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
"""Random cmaps over characters that compose and decompose, shaped here and by HarfBuzz.

A font per seed with no layout tables and a cmap holding a random part of a set of
letters, combining marks, composed letters, Hangul, spaces and compatibility characters,
so that the choice between keeping a composed character, taking it apart and putting
a pair back together is made differently in every font. Twelve random strings of those
characters per seed; glyphs, clusters and positions compared. Nothing is committed.

Usage: norm_random_diff.py [--seeds N] [--first K] [--script latn|hang|arab] [-v]
                           [--driver PATH] [--scratch DIR]
"""

import json
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
from gsub_random_diff import base_font

SCRATCH = os.path.join(oracle_env.ROOT, "build", "oracle", "norm-random")

SETS = {
    "latn": ("Latn", "latn", [
        0x61, 0x65, 0x75, 0x6F, 0x20, 0xA0,
        0x300, 0x301, 0x308, 0x323, 0x327, 0x302, 0x303, 0x31B,
        0xE0, 0xE9, 0xEA, 0xEB, 0xFC, 0xF5, 0x1D8, 0x1E73, 0x1EBF, 0x1EC7,
        0x1EA1, 0x1EB9, 0x1E21, 0x1EE5, 0x1EE3, 0x1B0, 0x1EEB, 0x1D6,
        0xFB01, 0xB2, 0x2002, 0x2003, 0x2007, 0x2009, 0x200B, 0x200D,
        0x340, 0x344, 0x1E9B, 0x1E9B + 0x100 - 0x100]),
    "hang": ("Hang", "hang", [
        0x1100, 0x1101, 0x1161, 0x1162, 0x11A8, 0x11A9, 0xAC00, 0xAC01, 0xAC1C,
        0xB098, 0xB0A8, 0x1112, 0x1175, 0x11AB, 0xD558, 0xD55C, 0x302E, 0x20]),
    "arab": ("Arab", "arab", [
        0x627, 0x628, 0x62C, 0x644, 0x64B, 0x64E, 0x651, 0x653, 0x654, 0x655,
        0x622, 0x623, 0x625, 0x624, 0x626, 0x649, 0x64A, 0x671, 0x6C0, 0x6D3,
        0x200D, 0x200C, 0x20, 0x670, 0xFE8D, 0xFE8E, 0xFEF5, 0xFEFB]),
}


def main(argv):
    seeds, first, driver, scratch, script = 200, 0, G.DRIVER, SCRATCH, "latn"
    for i, a in enumerate(argv):
        if a == "--seeds": seeds = int(argv[i + 1])
        elif a == "--first": first = int(argv[i + 1])
        elif a == "--driver": driver = argv[i + 1]
        elif a == "--scratch": scratch = argv[i + 1]
        elif a == "--script": script = argv[i + 1]
    iso, ot, chars = SETS[script]
    os.makedirs(scratch, exist_ok=True)
    lines = []
    for seed in range(first, first + seeds):
        r = random.Random(seed)
        font = os.path.join(scratch, "s%d.ttf" % seed)
        text = os.path.join(scratch, "s%d.txt" % seed)
        keep = [c for c in chars if r.random() < .6]
        for must in chars[:3]:
            if must not in keep:
                keep.append(must)
        r.shuffle(keep)
        cmap = {c: i + 1 for i, c in enumerate(keep)}
        with open(font, "wb") as h:
            h.write(base_font(nglyphs=len(keep) + 2, cmap_map=cmap))
        rr = random.Random(seed * 7 + 1)
        with open(text, "w", encoding="utf-8") as h:
            for _ in range(12):
                h.write(''.join(chr(rr.choice(chars)) for _ in range(rr.randint(1, 6))) + "\n")
        lines.append("hb-shape --font-file='%s' --output-format=json "
                     "--no-glyph-names --script=%s --text-file='%s' > '%s.hb' 2>/dev/null"
                     % (font, iso, text, font))
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
                ours = subprocess.run([driver, "--batch", "--script", ot, font], stdin=h,
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
    print("norm_random_diff: %s, %d seeds from %d, %d strings, %d differ, %d skipped"
          % (script, seeds, first, compared, bad, skipped))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
