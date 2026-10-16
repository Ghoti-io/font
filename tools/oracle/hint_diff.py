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
"""Hinted outlines, this library against FreeType, point by point.

documentation/design.md section 8.5. FreeType is the only other implementation
that runs TrueType bytecode, so it is the oracle: for each glyph of each font, at
each size and interpreter version, the outline after hinting is compared as
integers in 26.6, with no tolerance. A one-pixel move that shows only at one
size is exactly the failure an instruction bug makes, so the sizes include
awkward ones.

What is compared is a glyph this library hinted: one it declined (a composite
before composites are done, a program it refuses) is counted and not compared,
and a glyph FreeType refused is counted too.

Usage:
    hint_diff.py [--version 35|40] [--ppem N[,N...]] [--stride N] [--fonts N]
                 [--advance] [--quiet] [font...]
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-hint")


# Differences known and explained, keyed by (font file, interpreter version, ppem,
# glyph). Asserted both ways, as the gates here do for a skip: a listed glyph that
# now agrees fails the run (remove the entry), and a difference not listed fails
# it too.
# How far apart two hinted points may be, in 64ths of a pixel, when the program
# turned a direction into a unit vector. A bound and not a pass: a glyph further
# apart than this is a difference.
OBLIQUE_BOUND = 4

EXPECTED = {}


def parse(text):
    """{glyph: (advance, [(x, y, on)...], [ends]) | 'unhinted ...' | 'error ...'}"""
    glyphs = {}
    current = None
    for line in text.splitlines():
        if line.startswith("g"):
            fields = line.split()
            key = int(fields[0][1:])
            if fields[1] == "adv":
                current = (int(fields[2]), [], [], {}, "oblique" in fields)
                glyphs[key] = current
            else:
                glyphs[key] = " ".join(fields[1:])
                current = None
        elif current is not None and line.startswith("e"):
            current[2].extend(int(v) for v in line.split()[1:])
        elif current is not None and line.startswith("p "):
            _, x, y, on = line.split()[:4]
            touch = line.split()[4] if len(line.split()) > 4 else "t0"
            current[1].append((int(x), int(y), int(on)))
            current[3][len(current[1]) - 1] = int(touch[1:])
    return glyphs


PROGRAMS = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                        "font-program")


def has_control_value_program(path):
    """Whether the font has a `prep` table.

    FreeType hints a font that has none from a zeroed graphics state - no
    auto-flip, a minimum distance of 0, a control value cut-in of 0 - and not from
    the specification's defaults, which this library follows. A font like that
    (misaki_mincho.ttf is one) renders differently by design, and a comparison of
    the two would measure that and nothing else.
    """
    done = subprocess.run([PROGRAMS, path, "0", "1000000"], capture_output=True, text=True)
    return "== prep" in done.stdout


def ours(path, version, ppem, stride):
    done = subprocess.run([DRIVER, path, "0", str(version), str(ppem),
                           str(stride), "0"], capture_output=True, text=True)
    if done.returncode != 0:
        return None, done.stderr.strip()[-200:]
    return parse(done.stdout), ""


def theirs(path, version, ppem, stride):
    argv = oracle_env.command("freetype", ["freetype-hinted", path, "0",
                                            str(version), str(ppem),
                                            str(stride), "0"])
    done = subprocess.run(argv, capture_output=True, text=True)
    if done.returncode != 0:
        return None, done.stderr.strip()[-200:]
    return parse(done.stdout), ""


def main(argv):
    version = 35
    ppems = [11, 12, 13, 16, 17, 22]
    stride = 7
    fonts = None
    advance = False
    quiet = False
    paths = []
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--version":
            version = int(args.pop(0))
        elif a == "--ppem":
            ppems = [int(v) for v in args.pop(0).split(",")]
        elif a == "--stride":
            stride = int(args.pop(0))
        elif a == "--fonts":
            fonts = int(args.pop(0))
        elif a == "--advance":
            advance = True
        elif a == "--quiet":
            quiet = True
        else:
            paths.append(a)
    sampled = not paths
    if sampled:
        paths = corpus.fonts("sfnt")
        # Every few fonts, so the sample is spread over the corpus rather than
        # the first foundry's, and only fonts with programs to compare count.
        paths = paths[::7]

    totals = {"compared": 0, "same": 0, "different": 0, "declined": 0,
              "refused": 0, "advance": 0, "expected": 0, "oblique": 0, "no_prep": 0}
    expected_seen = set()
    shown = 0
    reasons = {}
    kinds = {}
    used = 0
    for path in paths:
        if fonts and used >= fonts:
            break
        before = totals["compared"]
        if not has_control_value_program(path):
            totals["no_prep"] += 1
            continue
        for ppem in ppems:
            mine, why = ours(path, version, ppem, stride)
            if mine is None:
                continue
            yours, why = theirs(path, version, ppem, stride)
            if yours is None:
                continue
            for glyph, answer in sorted(mine.items()):
                if isinstance(answer, str):
                    totals["declined"] += 1
                    reasons[answer] = reasons.get(answer, 0) + 1
                    continue
                other = yours.get(glyph)
                if other is None or isinstance(other, str):
                    totals["refused"] += 1
                    continue
                totals["compared"] += 1
                if answer[1] == other[1] and answer[2] == other[2]:
                    totals["same"] += 1
                    if (os.path.basename(path), version, ppem, glyph) in EXPECTED:
                        print("%s: %d ppem %d glyph %d now agrees with FreeType; remove it from EXPECTED"
                              % (os.path.basename(path), version, ppem, glyph))
                        totals["different"] += 1
                elif (os.path.basename(path), version, ppem, glyph) in EXPECTED:
                    expected_seen.add((os.path.basename(path), version, ppem, glyph))
                    totals["expected"] += 1
                elif answer[4] and max(abs(a[k] - b[k]) for a, b in zip(answer[1], other[1]) for k in (0, 1)) <= OBLIQUE_BOUND:
                    # A program that makes a vector from a direction gets FreeType's
                    # approximation of it, good to a 16384th; the difference it
                    # makes is a few 64ths of a pixel and no more.
                    totals["oblique"] += 1
                else:
                    totals["different"] += 1
                    for i, (a, b) in enumerate(zip(answer[1], other[1])):
                        if a == b:
                            continue
                        t = answer[3].get(i, 0)
                        for axis in (0, 1):
                            if a[axis] != b[axis]:
                                key = "%s %s" % ("xy"[axis], "touched" if t & (1 << axis) else "untouched")
                                kinds[key] = kinds.get(key, 0) + 1
                    if shown < 12:
                        shown += 1
                        where = next((i for i, (a, b) in enumerate(zip(answer[1], other[1])) if a != b), None)
                        print("%s ppem %d v%d glyph %d: %s" % (
                            os.path.basename(path), ppem, version, glyph,
                            ("point %d ours %s theirs %s" % (where, answer[1][where], other[1][where]))
                            if where is not None else
                            "%d points vs %d" % (len(answer[1]), len(other[1]))))
                if advance and answer[0] != other[0]:
                    totals["advance"] += 1
        if totals["compared"] > before:
            used += 1
    print("compared %(compared)d glyphs: %(same)d identical, %(different)d different; "
          "%(declined)d declined by this library, %(refused)d refused by FreeType, "
          "%(advance)d advance differences; %(expected)d expected differences; %(oblique)d within the oblique-vector bound; %(no_prep)d fonts with no prep left out" % totals)
    for key, count in sorted(kinds.items()):
        print("  differing coordinate: %d x %s" % (count, key))
    if not quiet:
        for why, count in sorted(reasons.items(), key=lambda kv: -kv[1])[:8]:
            print("  declined %d x %s" % (count, why))
    return 1 if totals["different"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
