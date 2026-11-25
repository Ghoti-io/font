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
"""Plant a defect in the automatic hinter and see FreeType's comparison refuse it.

Each mutant is one change to src/hint/autofit.c (a rounding rule, a threshold, a fitted constant). It
is killed when either sample of the comparison identifies a different number of glyphs or points as
identical to FreeType's than the unmutated build does, or, failing that, when the unit tests refuse it.
The samples: the synthetic letter boxes (exact), and the letters of the corpus faces.

The half-pixel step of an overshoot's rounding is not mutated: a zone is inactive past 0.75 pixel, so the
step's rounding can differ only for an overshoot of exactly that, and no size of a face reaches it.

The source is restored and the driver rebuilt after every mutant, and a control run of the unmutated
tree comes first and must be stable.

Usage: autohint_mutation_check.py   (PREFIX and BUILD as for make)
"""

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PREFIX = os.environ.get("PREFIX", str(ROOT.parents[1] / ".local"))
BUILD = os.environ.get("BUILD", "linux/release")
MAKE_BUILD = BUILD.removeprefix("linux/")
APPS = ROOT / "build" / BUILD / "apps"
DRIVER = APPS / "examples" / "font-hint-cff"
ARCHIVE = APPS / "libghoti.io-font-0.a"
SOURCE = "src/hint/autofit.c"

MUTANTS = [
    ("#define AF_MIN_SEGMENT_DIV 100", "#define AF_MIN_SEGMENT_DIV 20", "a run under a twentieth of an em is no segment"),
    ("if (ax > 14 * ay) {", "if (ax > 12 * ay) {", "a segment's direction needs a ratio of 12 and not 14"),
    ("if (overlap * 2048 < 12 * up) {", "if (overlap * 2048 < 24 * up) {", "two segments link only on twice the overlap"),
    ("if (threshold > 16) {", "if (threshold > 8) {", "edges merge within eight sixty-fourths at most"),
    ("return li + lo - lt < (lt >> 4);", "return li + lo - lt < (lt >> 3);", "a flat corner is judged at an eighth of its length"),
    ("z->active = dist <= 48 && dist >= -48;", "z->active = dist <= 32 && dist >= -32;", "a blue zone is inactive past half a pixel"),
    ("if (delta2 < 32) {", "if (delta2 < 16) {", "an overshoot under half a pixel is flattened only below a quarter"),
    ("fitted = (scaled + 40) & ~(int64_t)63;", "fitted = (scaled + 32) & ~(int64_t)63;", "the x-height rounds up from a half and not from 0.375"),
    ("if ((moved < 0 ? -moved : moved) < 128) {", "if ((moved < 0 ? -moved : moved) < 192) {", "the scale correction is skipped at three pixels and not two"),
    ("int64_t u = cur_len <= 64 ? 32 : 38;", "int64_t u = cur_len <= 64 ? 32 : 32;", "a wide stem centres on the grid the way a narrow one does (upper)"),
    ("int64_t d = cur_len <= 64 ? 32 : 26;", "int64_t d = cur_len <= 64 ? 32 : 32;", "a wide stem centres on the grid the way a narrow one does (lower)"),
    ("if (best_limit > 32) {", "if (best_limit > 16) {", "a serif is held to a quarter pixel and not half"),
    ("if (delta < 64 + 48) {", "if (delta < 64 + 8) {", "a serif is recognised at a smaller distance"),
    ("e->pos = g->edges[anchor].pos + ((e->opos - g->edges[anchor].opos + 16) & ~(int64_t)31);",
     "e->pos = g->edges[anchor].pos + ((e->opos - g->edges[anchor].opos + 8) & ~(int64_t)31);",
     "an edge left to the half-pixel snap rounds at a quarter and not a half"),
    ("    return &af->scripts[af->script_of[glyph]];", "    return &af->scripts[AF_LATIN];",
     "a Greek glyph is fitted with the Latin zones"),
    ("{0x370, 0x3FF, 0x1F00, 0x1FFF, 0, 0, 0, 0, 0, 0},", "{0x3C0, 0x3FF, 0x1F00, 0x1FFF, 0, 0, 0, 0, 0, 0},",
     "the Greek capitals are not mapped to the Greek script"),
    ("{0x590, 0x5FF, 0xFB1D, 0xFB4F, 0, 0, 0, 0, 0, 0},", "{0x5D0, 0x5E0, 0xFB1D, 0xFB4F, 0, 0, 0, 0, 0, 0},",
     "the Hebrew letters after lamed are not mapped to the Hebrew script"),
    ("{0x600, 0x6FF, 0x750, 0x77F, 0xFB50, 0xFDFF, 0xFE70, 0xFEFF, 0, 0},",
     "{0x640, 0x6FF, 0x750, 0x77F, 0xFB50, 0xFDFF, 0xFE70, 0xFEFF, 0, 0},",
     "the Arabic letters before tatweel are not mapped to the Arabic script"),
    ("{0x10D0, 0x10FF, 0x1C90, 0x1CBF, 0x2D00, 0x2D2F, 0, 0, 0, 0},",
     "{0x10E0, 0x10FF, 0x1C90, 0x1CBF, 0x2D00, 0x2D2F, 0, 0, 0, 0},",
     "the first Georgian letters are not mapped to the Georgian script"),
    ("{0x530, 0x58F, 0xFB13, 0xFB17, 0, 0, 0, 0, 0, 0},", "{0x560, 0x58F, 0xFB13, 0xFB17, 0, 0, 0, 0, 0, 0},",
     "the Armenian capitals are not mapped to the Armenian script"),
    ("&& ((script->zones[z].shoot > script->zones[z].ref) != af_zone_top[z])) {", "&& false) {",
     "an overshoot on the wrong side of its flat is kept, not averaged away"),
]

SAMPLES = [
    ("boxes", ["freetype", "--", "python3", "tools/oracle/hint_cff_diff.py", "--auto", "--pool", "sfnt", "--files",
               "tests/data/fonts/latin-boxes.ttf", "--ppem", "9,10,11,12,13,14,16,17,20,24,32", "--stride", "1"]),
    ("tall", ["freetype", "--", "python3", "tools/oracle/hint_cff_diff.py", "--auto", "--pool", "sfnt", "--files",
              "tests/data/fonts/latin-boxes-tall.ttf", "--ppem", "8,9,10,11,12,14,16,20,24", "--stride", "1"]),
    ("greek", ["freetype", "--", "python3", "tools/oracle/hint_cff_diff.py", "--auto", "--pool", "sfnt", "--files",
               "tests/data/fonts/latin-greek-boxes.ttf", "--ppem", "8,9,10,11,12,13,14,16,17,20,24,32", "--stride", "1"]),
    ("letters", ["fonttools,freetype", "--", "python3", "tools/oracle/autohint_diff.py", "--show", "0"]),
    ("noto", ["fonttools,freetype", "--", "python3", "tools/oracle/autohint_diff.py", "--script", "latin", "--show", "0",
              "NotoSans-Regular.ttf", "NotoSans-Bold.ttf"]),
]


def run(command):
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True)


def rebuild():
    done = run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(DRIVER.relative_to(ROOT))])
    if done.returncode != 0:
        raise RuntimeError("the build failed:\n" + (done.stdout + done.stderr)[-800:])


def forget():
    obj = ROOT / "build" / BUILD / "objects" / "hint" / "autofit.o"
    for artifact in (obj, ARCHIVE, DRIVER):
        artifact.unlink(missing_ok=True)


def unit_tests_refuse():
    built = run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str((APPS / "testAutofit").relative_to(ROOT))])
    if built.returncode != 0:
        raise RuntimeError("the unit tests did not build:\n" + (built.stdout + built.stderr)[-800:])
    env = {**os.environ, "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH", "") + ":" + PREFIX + "/lib/ghoti.io"}
    done = subprocess.run([str(APPS / "testAutofit"), "--gtest_brief=1"], cwd=ROOT, capture_output=True, text=True, env=env)
    return done.returncode != 0


def measure():
    out = {}
    for label, args in SAMPLES:
        done = run(["python3", "tools/oracle/oracle_run.py", *args])
        match = re.search(r"compared (\d+) glyphs: (\d+) identical.*?(?:points (\d+) of \d+ identical)?", done.stdout)
        if not match:
            raise RuntimeError(f"sample '{label}' printed no summary:\n{(done.stdout + done.stderr)[-500:]}")
        points = re.search(r"points (\d+) of", done.stdout)
        out[label] = (int(match.group(1)), int(match.group(2)), int(points.group(1)) if points else 0)
    return out


def main():
    forget()
    rebuild()
    baseline = measure()
    if measure() != baseline:
        print("autohint_mutation_check: the control run is not stable", file=sys.stderr)
        return 1
    print("control:", ", ".join(f"{k} {v[1]}/{v[0]} (points {v[2]})" for k, v in baseline.items()))
    survived = []
    path = ROOT / SOURCE
    original = path.read_text()
    try:
        for needle, replacement, what in MUTANTS:
            if original.count(needle) != 1:
                raise RuntimeError(f"the needle for '{what}' matches {original.count(needle)} times, not once")
            try:
                path.write_text(original.replace(needle, replacement))
                forget()
                rebuild()
                got = measure()
            finally:
                path.write_text(original)
            killed_by = [k for k in baseline if got[k] != baseline[k]]
            if not killed_by:
                try:
                    path.write_text(original.replace(needle, replacement))
                    forget()
                    if unit_tests_refuse():
                        killed_by = ["unit tests"]
                finally:
                    path.write_text(original)
            if killed_by:
                print(f"killed   {what} ({', '.join(killed_by)})")
            else:
                print(f"SURVIVED {what}")
                survived.append(what)
    finally:
        path.write_text(original)
        forget()
        rebuild()
    if survived:
        print(f"autohint_mutation_check: {len(survived)} mutant(s) survived", file=sys.stderr)
        return 1
    print(f"autohint_mutation_check: all {len(MUTANTS)} mutants were refused by the comparison")
    return 0


if __name__ == "__main__":
    sys.exit(main())
