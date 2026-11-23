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
"""Plant a defect in the CFF and Type 1 hinter and see FreeType's comparison refuse it.

A differential that has only ever passed has not been shown able to fail, and `hint_cff_diff.py`
reports a fraction, which a defect can leave looking fine. Each mutant is one change to the engine
(a rounding rule, a fitted constant, the merge of an accented character's hints); it is killed when
*any* of seven samples of the comparison identifies a different number of glyphs as identical to FreeType's, or of advances as different, than the
unmutated build does. The samples are small and fixed: the accented-character fixtures, the one font
whose widest blue zone changes BlueScale, the one whose BlueScale is large, two corpus fonts at two sizes, and two Type 1 programs.

The source is restored and the driver rebuilt after every mutant, and a control run of the
unmutated tree comes first and must be stable (two runs, the same figures).

Usage: hint_cff_mutation_check.py   (PREFIX and BUILD as for make)
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

MUTANTS = [
    ("src/hint/cff_hint.c", "#define GFNT_CFFH_BOOST_NUM 6", "#define GFNT_CFFH_BOOST_NUM 5",
     "the outward bias at the smallest size is 0.5 pixel and not 0.6"),
    ("src/hint/cff_hint.c", "#define GFNT_CFFH_OVERSHOOT_BIAS 13107", "#define GFNT_CFFH_OVERSHOOT_BIAS 6554",
     "an overshoot kept above BlueScale is biased by 0.1 pixel and not 0.2"),
    ("src/hint/cff_hint.c", "#define GFNT_CFFH_BOOST_MAX 0x8000", "#define GFNT_CFFH_BOOST_MAX 0x6000",
     "the outward bias is capped at 0.375 pixel and not 0.5"),
    ("src/hint/cff_hint.c", "#define GFNT_CFFH_MIN_GAP 0x8000", "#define GFNT_CFFH_MIN_GAP 0x6000",
     "a free stem keeps 0.375 pixel from the edge below and not 0.5"),
    ("src/hint/cff_hint.c", "#define GFNT_CFFH_TIE 0", "#define GFNT_CFFH_TIE 0x8000",
     "a stem edge a half pixel from the grid is rounded as if it were nearer"),
    ("src/hint/cff_hint.c", "  return q - r;\n}", "  return q - r + 1;\n}",
     "the whole-pixel rounding is off by one sixty-fifth-thousandth"),
    ("src/hint/cff_hint.c", "if (units < shift) {", "if (units <= shift) {",
     "an overshoot of exactly BlueShift is flattened"),
    ("src/hint/cff_hint.c", "if (limit < scale) {", "if (limit < 0) {",
     "BlueScale is not limited by the widest blue zone"),
    ("src/hint/cff_hint.c", ">> 3);", ">> 2);",
     "a stem below the baseline fades over four pixels and not eight"),
    ("src/hint/cff_hint.c",
     "fine_x[first + n - 1u] == fine_x[first] && fine_y[first + n - 1u] == fine_y[first]",
     "points[first + n - 1u].x == points[first].x && points[first + n - 1u].y == points[first].y",
     "the closing point is compared after the 26.6 truncation"),
    ("src/hint/cff_hint.c", "const bool accent_first = hints.accent && gfnt_sfnt_producer(face) != GFNT_PRODUCER_TYPE1;",
     "const bool accent_first = false;",
     "a CFF accent's contours stay after its base's"),
    ("src/hint/cff_hint.c", "scaled = scaled >= 0 ? ((scaled + 32) / 64) * 64 :",
     "scaled = scaled >= 0 ? ((scaled + 31) / 64) * 64 :",
     "the advance's rounding of a half pixel goes down"),
    ("src/hint/cff_hint.c", "#define GFNT_CFFH_CLUSTER_GAP 0xC000", "#define GFNT_CFFH_CLUSTER_GAP 0x4000",
     "stems closer than a quarter pixel are fitted together and the rest one at a time"),
    ("src/hint/cff_hint.c", "#define GFNT_CFFH_UNALIGNED_COST 0x20000", "#define GFNT_CFFH_UNALIGNED_COST 0",
     "leaving a stem unaligned costs nothing"),
    ("src/hint/cff_hint.c", "if (!free_choice && bottom[i][c] - (bottom[i - 1u][p] + width[i - 1u]) < GFNT_CFFH_MIN_GAP) {",
     "if (!free_choice && bottom[i][c] - (bottom[i - 1u][p] + width[i - 1u]) < 0) {",
     "two stems fitted together may come within half a pixel of each other"),
    ("src/charstring/charstring.c", "+ (part->stems[k].vertical ? dx : dy));", "+ (part->stems[k].vertical ? dx : dy / 2));",
     "an accent's stems are not moved by its offset"),
    ("src/charstring/charstring.c", "m->first_point = (uint32_t)(first_point + part->masks[i].first_point);",
     "m->first_point = (uint32_t)(part->masks[i].first_point);",
     "an accent's masks start at the beginning of the glyph"),
    ("src/charstring/charstring.c", "gfnt_cs_mask_set(m->bits, first_stem + k);\n    }\n  }\n  for (size_t i = 0;",
     "gfnt_cs_mask_set(m->bits, k);\n    }\n  }\n  for (size_t i = 0;",
     "a component with no mask of its own switches on the first component's stems"),
]

# (label, arguments of hint_cff_diff.py)
SAMPLES = [
    ("accents", ["--pool", "sfnt", "--files",
                 "tests/data/fonts/cff-seac-hints.otf,tests/data/fonts/cff-seac.otf", "--ppem", "9,12,15,32",
                 "--stride", "1"]),
    ("type1 accents", ["--pool", "type1", "--files",
                       "tests/data/fonts/type1-seac-hints.pfb,tests/data/fonts/type1.pfb", "--ppem", "9,12,15,32",
                       "--stride", "1"]),
    ("stems", ["--pool", "sfnt", "--files", "tests/data/fonts/cff-stems.otf", "--ppem", "9,17", "--stride", "1"]),
    ("schola", ["--pool", "sfnt", "--only", "texgyreschola-math.otf", "--ppem", "9,32", "--stride", "19"]),
    ("cantarell", ["--pool", "sfnt", "--only", "Cantarell-VF.otf", "--ppem", "9,12", "--stride", "5"]),
    ("cff", ["--pool", "sfnt", "--fonts", "3", "--ppem", "12,17", "--stride", "29"]),
    ("type1", ["--pool", "type1", "--fonts", "2", "--ppem", "12,32", "--stride", "31"]),
]


def run(command):
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True)


def rebuild():
    done = run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(DRIVER.relative_to(ROOT))])
    if done.returncode != 0:
        raise RuntimeError("the build failed:\n" + (done.stdout + done.stderr)[-800:])


def forget(source):
    obj = ROOT / "build" / BUILD / "objects" / Path(source).parent.name / (Path(source).stem + ".o")
    for artifact in (obj, ARCHIVE, DRIVER):
        artifact.unlink(missing_ok=True)


def unit_tests_refuse():
    """Whether the CFF unit tests fail on the tree as it stands."""
    built = run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str((APPS / "testCff").relative_to(ROOT))])
    if built.returncode != 0:
        raise RuntimeError("the unit tests did not build:\n" + (built.stdout + built.stderr)[-800:])
    env = {**os.environ, "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH", "") + ":" + PREFIX + "/lib/ghoti.io"}
    done = subprocess.run([str(APPS / "testCff"), "--gtest_brief=1"], cwd=ROOT, capture_output=True, text=True, env=env)
    return done.returncode != 0


def measure():
    """{sample: (glyphs, identical, advances that differ)}, from hint_cff_diff.py's own summary line."""
    out = {}
    for label, args in SAMPLES:
        done = run(["python3", "tools/oracle/oracle_run.py", "freetype", "--", "python3",
                    "tools/oracle/hint_cff_diff.py", *args])
        match = re.search(r"compared (\d+) glyphs: (\d+) identical.*?(\d+) advances differ", done.stdout)
        if not match:
            raise RuntimeError(f"sample '{label}' printed no summary:\n{(done.stdout + done.stderr)[-500:]}")
        out[label] = (int(match.group(1)), int(match.group(2)), int(match.group(3)))
    return out


def main():
    forget("src/hint/cff_hint.c")
    rebuild()
    baseline = measure()
    if measure() != baseline:
        print("hint_cff_mutation_check: the control run is not stable", file=sys.stderr)
        return 1
    print("control:", ", ".join(f"{k} {v[1]}/{v[0]}" for k, v in baseline.items()))
    survived = []
    try:
        for source, needle, replacement, what in MUTANTS:
            path = ROOT / source
            original = path.read_text()
            if original.count(needle) != 1:
                raise RuntimeError(f"{source}: the needle for '{what}' matches {original.count(needle)} times, not once")
            try:
                path.write_text(original.replace(needle, replacement))
                forget(source)
                rebuild()
                got = measure()
            finally:
                path.write_text(original)
            killed_by = [k for k in baseline if got[k] != baseline[k]]
            if not killed_by:
                # What FreeType's comparison cannot see, the unit tests may: build them against the mutant.
                try:
                    path.write_text(original.replace(needle, replacement))
                    forget(source)
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
        forget("src/hint/cff_hint.c")
        forget("src/charstring/charstring.c")
        rebuild()
    if survived:
        print(f"hint_cff_mutation_check: {len(survived)} mutant(s) survived", file=sys.stderr)
        return 1
    print(f"hint_cff_mutation_check: all {len(MUTANTS)} mutants were refused by the comparison")
    return 0


if __name__ == "__main__":
    sys.exit(main())
