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
"""Plant a defect in where a `glyf` glyph's origin is and see the tests reject it.

A `glyf` outline is moved so that the glyph's origin, the first phantom point
(`xMin - lsb`), is at x = 0 (src/glyf/glyf.c; design.md section 7.3, "Where a
glyph's outline sits"). The rule has four parts that a reader can get wrong
independently: the point itself and the direction of the move, which glyph's point a
composite uses (its own, or the USE_MY_METRICS component's, the last flagged one, and
through nesting), what `gvar` and `HVAR` do to it at a location, and which callers
want the points unmoved (the hinter). Each mutant below is one literal replacement in
src/glyf/glyf.c or its callers; it must match exactly once and be refused by
testPlacement, testGlyf, testVariation or the hinting units. The file is restored and
the artifacts rebuilt after every mutant, whatever happened.

The comparisons with FreeType and fontTools (`make check-oracle-glyf` and
`check-oracle-var`) are the other half of the evidence and are not run here: they
need a container engine, and a mutant the units refuse is a mutant they would too,
since they compare the same points glyph by glyph.

What is NOT planted, and why:

  - the saturation of a moved coordinate: a point within 2^31 of the limit of 26.6
    is not a point of any font the units can write
  - the out-of-memory arms of the placement: the allocation sweeps run through
    them, but a defect planted in an error return reads as a different failure and
    not as a moved glyph
  - a CFF, CFF2 or Type 1 outline moved: those producers never call this code, so
    there is no line to mutate; the unit that holds them unmoved (`cff.otf`, whose
    bearings are zero and whose glyphs start at 40, 80 and 60) is what a producer
    that started to would fail

Usage: placement_mutation_check.py   (PREFIX and BUILD as for make)
"""

import glob
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PREFIX = os.environ.get("PREFIX", str(ROOT.parents[1] / ".local"))
BUILD = os.environ.get("BUILD", "linux/release")
MAKE_BUILD = BUILD.removeprefix("linux/")
APPS = ROOT / "build" / BUILD / "apps"
OBJECTS = ROOT / "build" / BUILD / "objects"
LD_PATH = f"{PREFIX}/lib/ghoti.io"
UNITS = [APPS / "testPlacement", APPS / "testGlyf", APPS / "testVariation",
         APPS / "testHint_glyph", APPS / "testHint_variation"]

GLYF = "src/glyf/glyf.c"
HINT = "src/hint/glyph.c"

MUTANTS = [
    # The move itself.
    (GLYF, "    outline->points[i].x = gfnt_saturate32((int64_t)outline->points[i].x - pp1);",
     "    outline->points[i].x = gfnt_saturate32((int64_t)outline->points[i].x);",
     "the move dropped"),
    (GLYF, "    outline->points[i].x = gfnt_saturate32((int64_t)outline->points[i].x - pp1);",
     "    outline->points[i].x = gfnt_saturate32((int64_t)outline->points[i].x + pp1);",
     "the move the wrong way"),
    (GLYF, "  *out = ((int64_t)x_min - (int64_t)bearing) * GFNT_F26DOT6_ONE;",
     "  *out = ((int64_t)bearing - (int64_t)x_min) * GFNT_F26DOT6_ONE;",
     "the origin taken as the bearing less xMin"),
    (GLYF, "  *out = ((int64_t)x_min - (int64_t)bearing) * GFNT_F26DOT6_ONE;",
     "  *out = (int64_t)x_min * GFNT_F26DOT6_ONE;",
     "the bearing forgotten"),
    (GLYF, "  *out = ((int64_t)x_min - (int64_t)bearing) * GFNT_F26DOT6_ONE;",
     "  *out = -(int64_t)bearing * GFNT_F26DOT6_ONE + 0 * (int64_t)x_min;",
     "xMin forgotten"),
    (GLYF, "  if (result != GFNT_OK || !place || pp1 == 0) {",
     "  if (result != GFNT_OK || pp1 == 0) {",
     "the hinter's unmoved points moved"),
    (HINT, "r = gfnt_face_glyph_outline_unplaced(w->face, glyph, NULL, NULL, &units, w->error);",
     "r = gfnt_face_glyph_outline(w->face, glyph, NULL, NULL, &units, w->error);",
     "the hinter given moved points"),
    # Only the outline this call appended is moved.
    (GLYF, "  for (size_t i = first; i < outline->point_count; ++i) {",
     "  for (size_t i = 0; i < outline->point_count; ++i) {",
     "points an outline already held moved again"),
    # A composite: whose point.
    (GLYF, "    if (want_pp1 && (component.flags & GFNT_GLYF_USE_MY_METRICS)) {",
     "    if (0) {",
     "USE_MY_METRICS ignored"),
    (GLYF, "      *out_has_borrowed = true;\n      *out_borrowed = part_pp1;",
     "      if (!*out_has_borrowed) {\n        *out_borrowed = part_pp1;\n      }\n      *out_has_borrowed = true;",
     "the first flagged component wins and not the last"),
    (GLYF, "      *out_has_borrowed = true;\n      *out_borrowed = part_pp1;",
     "      *out_has_borrowed = true;\n      *out_borrowed = 0;",
     "a flagged component lends the origin zero"),
    (GLYF, "    if (has_borrowed) {\n      *out_pp1 = borrowed;\n    }",
     "    if (0) {\n      *out_pp1 = borrowed;\n    }",
     "a composite always keeps its own point"),
    (GLYF, "      result = gfnt_glyf_stored_pp1(face, glyph, out_pp1, error);\n      if (result == GFNT_OK && component_deltas",
     "      result = gfnt_glyf_stored_pp1(face, glyph, out_pp1, error);\n      if (0 && component_deltas",
     "a composite's own gvar delta dropped"),
    (GLYF, "    return (want_pp1 && depth > 0)\n        ? gfnt_glyf_varied_pp1_bare(face, glyph, variation, out_pp1, error)\n        : GFNT_OK;\n  }\n  result = gfnt_read_s16",
     "    *out_pp1 = 0;\n    return GFNT_OK;\n  }\n  result = gfnt_read_s16",
     "an empty glyph lends no origin"),
    # At a location.
    (GLYF, "    if (result == GFNT_OK && variation\n        && !gfnt_glyf_hvar_owns_metrics(face, glyph, variation)) {\n      *out_pp1 += gfnt_glyf_delta_to_26dot6(delta);",
     "    if (result == GFNT_OK && variation\n        && !gfnt_glyf_hvar_owns_metrics(face, glyph, variation)) {\n      *out_pp1 -= gfnt_glyf_delta_to_26dot6(delta);",
     "gvar's delta to the origin the wrong way"),
    (GLYF, "    if (result == GFNT_OK && variation\n        && !gfnt_glyf_hvar_owns_metrics(face, glyph, variation)) {\n      *out_pp1 += gfnt_glyf_delta_to_26dot6(delta);",
     "    if (result == GFNT_OK && variation\n        && 0) {\n      *out_pp1 += gfnt_glyf_delta_to_26dot6(delta);",
     "gvar's delta to the origin dropped"),
    (GLYF, "  return gfnt_hvar_delta(face, glyph, GFNT_HVAR_ADVANCE, variation->coords,\n      variation->count, &advance, &scratch) == GFNT_OK;",
     "  return false;",
     "an HVAR never owns the metrics"),
    (GLYF, "  if (!variation || !gfnt_face_has_table(face, GFNT_TAG_HVAR)) {\n    return false;\n  }",
     "  if (!variation) {\n    return false;\n  }\n  if (!gfnt_face_has_table(face, GFNT_TAG_HVAR)) {\n    return true;\n  }",
     "a font with no HVAR taken to have one"),
    (GLYF, "  if (result != GFNT_OK || !variation\n      || gfnt_glyf_hvar_owns_metrics(face, glyph, variation)) {\n    return result;\n  }",
     "  if (result != GFNT_OK || !variation) {\n    return result;\n  }",
     "an empty glyph's origin moved by gvar under an HVAR"),
    # The faces with no bearing.
    (GLYF, "  if (result == GFNT_ERR_UNSUPPORTED) {\n    // No `hmtx`",
     "  if (0) {\n    // No `hmtx`",
     "a face with no hmtx refused its glyphs"),
    (GLYF, "  if (result != GFNT_OK) {\n    return result;\n  }\n  *out = ((int64_t)x_min",
     "  if (result != GFNT_OK) {\n    return GFNT_OK;\n  }\n  *out = ((int64_t)x_min",
     "a damaged hmtx leaves the glyph where its points are"),
]


def run(command, *, expect_failure=False):
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                          env={**os.environ, "LD_LIBRARY_PATH": LD_PATH})
    if (done.returncode != 0) != expect_failure:
        want = "failure" if expect_failure else "success"
        raise RuntimeError(f"expected {want} from {' '.join(map(str, command))} "
                           f"(exit {done.returncode}):\n{(done.stdout + done.stderr)[-800:]}")
    return done


def make(*targets):
    return run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", *targets])


def rebuild():
    make(*[str(u.relative_to(ROOT)) for u in UNITS])


def forget(source):
    obj = OBJECTS / Path(source).relative_to("src").with_suffix(".o")
    for artifact in [obj, *UNITS, *map(Path, glob.glob(str(APPS / "libghoti.io-font-*.a")))]:
        artifact.unlink(missing_ok=True)


def main():
    originals = {f: (ROOT / f).read_text() for f in {m[0] for m in MUTANTS}}
    caught_all = 0
    missed = []
    try:
        rebuild()
        for unit in UNITS:   # the control: the unmutated build passes
            run([str(unit), "--gtest_brief=1"])
        for source, needle, replacement, what in MUTANTS:
            original = originals[source]
            if original.count(needle) != 1:
                raise RuntimeError(f"{source}: the needle for '{what}' matches "
                                   f"{original.count(needle)} times, not once")
            if needle == replacement:
                raise RuntimeError(f"'{what}' changes nothing")
            path = ROOT / source
            caught = []
            try:
                path.write_text(original.replace(needle, replacement))
                forget(source)
                try:
                    rebuild()
                except RuntimeError as error:
                    raise RuntimeError(f"the mutant '{what}' does not build:\n{error}")
                for unit in UNITS:
                    done = subprocess.run([str(unit), "--gtest_brief=1"], cwd=ROOT,
                                          capture_output=True, text=True,
                                          env={**os.environ, "LD_LIBRARY_PATH": LD_PATH})
                    if done.returncode != 0:
                        caught.append(unit.name)
            finally:
                path.write_text(original)
                forget(source)
            if not caught:
                missed.append(what)
                print(f"placement_mutation_check: NOT CAUGHT: {what}", flush=True)
                continue
            caught_all += 1
            print(f"placement_mutation_check: rejected by {', '.join(caught)}: {what}",
                  flush=True)
        rebuild()
        if missed:
            raise RuntimeError(f"{len(missed)} mutants not caught: " + "; ".join(missed))
    except (OSError, RuntimeError) as error:
        for f, text in originals.items():
            (ROOT / f).write_text(text)
        print(f"placement_mutation_check: {error}", file=sys.stderr)
        return 1
    print(f"placement_mutation_check: {caught_all} of {len(MUTANTS)} mutants rejected, "
          "and the control passes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
