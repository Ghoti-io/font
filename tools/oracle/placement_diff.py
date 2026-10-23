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
"""Where each glyph of the placement fixtures sits, in this library and in three references.

documentation/design.md section 7.3, "Where a glyph's outline sits". The left edge of
the outline of every glyph of `outline-placement.ttf`, `variable-placement.ttf` and
`variable-placement-hvar.ttf` (the variable ones at `wght` +1 as well as the default),
as this library draws it, as **FreeType** draws it (unhinted, one pixel per font unit),
as **HarfBuzz** draws it (`hb-view`, SVG) and as **fontTools'** glyph set draws it.

The gate is this library against FreeType, **to the unit, glyph by glyph**: FreeType is
the reference renderer the rules are taken from. HarfBuzz and fontTools are printed and
not required to agree, because the table of where they part from FreeType (a composite,
which fontTools does not move; an `HVAR`, which FreeType alone lets own the origin) is
the design.md record, and a later reader who finds one of them changed will find it
here as a changed cell. Run it to regenerate that table.

Usage: placement_diff.py [--quiet]
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glyf_diff
import oracle_env

ROOT = oracle_env.ROOT
FIXTURES = glyf_diff.FIXTURES
DRIVER = glyf_diff.DRIVER
TABLE = os.path.join(ROOT, "tools", "oracle", "fonttools_placement_table.py")

# Glyph 2 onward: .notdef and space have nothing to place. Glyph k is U+0041 + k - 2.
NAMES = ["box", "moved", "back", "comp", "comp-umm", "comp-umm-first",
         "comp-umm-last", "comp-umm-nested", "comp-eq", "comp-umm-empty"]
# (fixture, the normalised wght coordinate or None for the default instance).
CASES = [("outline-placement.ttf", None), ("variable-placement.ttf", None),
         ("variable-placement.ttf", 1.0), ("variable-placement-hvar.ttf", None),
         ("variable-placement-hvar.ttf", 1.0)]


def run(argv):
    done = subprocess.run(argv, capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit("placement_diff: %s failed:\n%s"
                         % (" ".join(argv[:6]), done.stderr[-600:]))
    return done.stdout


def library(path, wght):
    """This library's left edges, from the outline driver's points."""
    argv = [DRIVER, path, "0", "1", "0"]
    if wght is not None:
        argv.append("wght=900")
    edges = {}
    for line in run(argv).splitlines():
        match = re.match(r"glyph (\d+) point \d+: (-?\d+) -?\d+ ", line)
        if match:
            glyph, x = int(match.group(1)), int(match.group(2))
            edges[glyph] = min(edges.get(glyph, x), x)
    return [edges[g] / 64 if g in edges else None for g in range(2, 12)]


def freetype(path, wght):
    argv = oracle_env.command("freetype", ["freetype-hinted", path, "0", "0",
        "1000", "1", "0", "normal", "-" if wght is None else "16384"])
    edges = {}
    glyph = None
    for line in run(argv).splitlines():
        if line.startswith("g"):
            glyph = int(line.split()[0][1:])
        elif line.startswith("p ") and glyph is not None:
            x = int(line.split()[1])
            edges[glyph] = min(edges.get(glyph, x), x)
    return [edges[g] / 64 if g in edges else None for g in range(2, 12)]


def harfbuzz(path, wght):
    out = []
    for glyph in range(2, 12):
        argv = ["hb-view", "--output-format=svg", "--font-size=1000", "--margin=0"]
        if wght is not None:
            argv.append("--variations=wght=900")
        text = run(oracle_env.command("harfbuzz", argv + [path, chr(0x41 + glyph - 2)]))
        xs = []
        for d in re.findall(r'<path d="([^"]*)"', text):
            tokens = d.split()
            i = 0
            while i < len(tokens):
                if tokens[i] in "MLQC":
                    i += 1
                    continue
                if tokens[i] == "Z":
                    i += 1
                    continue
                xs.append(float(tokens[i]))
                i += 2
        out.append(min(xs) if xs else None)
    return out


def fonttools(path, wght):
    argv = ["python3", TABLE, path] + ([] if wght is None else [str(wght)])
    out = run(oracle_env.command("fonttools", argv)).split()
    return [None if v == "-" else float(v) for v in out]


def cell(v):
    return "-" if v is None else "%g" % v


def main(argv):
    quiet = "--quiet" in argv
    if not os.path.exists(DRIVER):
        sys.stderr.write("%s is not built; run `make examples`\n" % DRIVER)
        return 1
    differences = 0
    compared = 0
    for fixture, wght in CASES:
        path = os.path.join(FIXTURES, fixture)
        ours = library(path, wght)
        theirs = {"FreeType": freetype(path, wght),
                  "HarfBuzz": harfbuzz(path, wght),
                  "fontTools": fonttools(path, wght)}
        label = "%s%s" % (fixture, "" if wght is None else " at wght +1")
        if not quiet:
            print("\n%s" % label)
            print("  %-16s %8s %9s %9s %9s" % ("glyph", "library", "FreeType",
                                               "HarfBuzz", "fontTools"))
        for i, name in enumerate(NAMES):
            compared += 1
            same = ours[i] == theirs["FreeType"][i]
            if not same:
                differences += 1
            if not quiet or not same:
                print("  %-16s %8s %9s %9s %9s%s" % (
                    name, cell(ours[i]), cell(theirs["FreeType"][i]),
                    cell(theirs["HarfBuzz"][i]), cell(theirs["fontTools"][i]),
                    "" if same else "   <-- this library and FreeType differ"))
    print("placement_diff: %d glyph(s) in %d case(s) against FreeType, %d "
          "difference(s); HarfBuzz and fontTools printed for the record"
          % (compared, len(CASES), differences))
    return 1 if differences or not compared else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
