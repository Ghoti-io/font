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
"""Vertical text in the variable fixtures at several locations, here and in HarfBuzz.

The fixtures have no `vmtx`, so a location moves what vertical text stands on: the
origin is the top phantom point, the glyph's stated top plus what `gvar` adds. Both
directions, five locations, four strings per run, every glyph compared.

Usage: variable_vertical_diff.py [-v]
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

ROOT = oracle_env.ROOT
SCRATCH = os.path.join(ROOT, "build", "oracle", "variable-vertical")
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples", "font-shape")
FONTS = ["variable-gvar.ttf", "variable-hvar.ttf", "variable-avar2.ttf", "variable-cvar.ttf",
         "variable-featurevars.ttf", "variable-stat.ttf", "variable-cff2.otf"]
LOCATIONS = ["wght=400", "wght=900", "wght=100,wdth=125", "wdth=75", "wght=575"]


def main(argv):
    os.makedirs(SCRATCH, exist_ok=True)
    txt = os.path.join(SCRATCH, "text.txt")
    with open(txt, "w") as h:
        h.write("ABA\nAB\nB\nBA\n")
    fixtures = os.path.join(ROOT, "tests", "data", "fonts")
    lines = []
    for f in FONTS:
        for i, loc in enumerate(LOCATIONS):
            for d in ("ttb", "btt"):
                lines.append("hb-shape --font-file='%s/%s' --output-format=json --no-glyph-names "
                             "--direction=%s --variations='%s' --text-file='%s' > '%s/%s_%d_%s.hb' 2>/dev/null"
                             % (fixtures, f, d, loc, txt, SCRATCH, f, i, d))
    runner = os.path.join(SCRATCH, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner], scratch=SCRATCH),
                         capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
    keys = ("g", "cl", "ax", "ay", "dx", "dy")
    norm = lambda g: [tuple(x.get(k) for k in keys) for x in g]
    bad = total = 0
    for f in FONTS:
        for i, loc in enumerate(LOCATIONS):
            for d in ("ttb", "btt"):
                with open("%s/%s_%d_%s.hb" % (SCRATCH, f, i, d), encoding="utf-8") as h:
                    hb = [json.loads(x) if x.startswith("[") else [] for x in h.read().split("\n")[:-1]]
                with open(txt, "rb") as h:
                    out = subprocess.run([DRIVER, "--batch", "--script", "latn", "--" + d, "--location", loc,
                                          os.path.join(fixtures, f)], stdin=h, capture_output=True,
                                         text=True).stdout
                mine = [json.loads(x) for x in out.split("\n")[:-1]]
                for a, b in zip(hb, mine):
                    total += 1
                    if not isinstance(b, list) or norm(a) != norm(b):
                        bad += 1
                        if "-v" in argv:
                            print(f, loc, d, norm(a)[:3], b if not isinstance(b, list) else norm(b)[:3])
    print("variable_vertical_diff: %d strings, %d differ" % (total, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
