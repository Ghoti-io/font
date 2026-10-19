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
"""Plant a defect in the rasteriser's glue and see the tests reject it.

The scan converter itself moved to shape-cpu (story 9 of the shape family's
first milestone), and shape-cpu has its own planted-defect gate. What stays in
src/raster/raster.c is what only a font can say: the frame an outline is placed
in (the y flip, the sub-pixel origin, the bitmap's corner), the walk that turns
an outline into a device path, and the view that turns shape-cpu's coverage back
into a GFNT_Coverage (`left` and `top` mapped back to y-up, the empty result).
A rendering that is wrong there is wrong in every pixel, and the committed
goldens and the unit tests must say so.

A gate that has only ever passed has not been shown to be able to fail (the
standing rule for every gate here). Each mutant below is one literal replacement
in src/raster/raster.c; it must match exactly once, and must be refused by the
unit tests (testRaster), the golden test (testGolden) or the flattener-glue test
(testFlatten). The file is restored
and the artifacts rebuilt after every mutant, whatever happened.

What is NOT planted, and why:

  - the `gshgeom_device_path_close` call in `gfnt_raster_close`, and the pen
    reset after it: shape-cpu fills an open subpath as if it were closed, so
    the closing line is drawn either way; the call is kept so that the device
    path says what the outline said
  - the mapping of a device-path `ERR_LIMIT` to `GFNT_ERR_LIMIT`, and of a
    shape-cpu `ERR_LIMIT`: the caps handed down are lifted (`SIZE_MAX`) or equal
    to the ones this function has just checked, so neither can be reached
  - the `memset` that empties `from` at the end of `gfnt_raster_view`: the
    caller never looks at it again (it is a local that is not destroyed)
  - the `options->tolerance < 0` clause of the tolerance check: the next clause
    (`!= 0 && < GFNT_RASTER_MIN_TOLERANCE`) refuses every negative value as well,
    so removing it changes nothing
  - the `default` arm of `gfnt_raster_check` and of the shape-cpu result: an
    error the device path or shape-cpu cannot return for an outline that has
    passed the bounds
  - the `gfnt_raster_check` of the flattener's own result: its one refusal of its
    own, a tolerance under the minimum, has been turned away by
    `gfnt_raster_outline` before any curve is walked
  - that the vertex sink stops the flattener at the first refusal (it returns
    the device path's result): the first failure is recorded either way and a
    later one is ignored, so the only difference is work not done

Usage: raster_mutation_check.py   (PREFIX and BUILD as for make)
"""

import glob
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PREFIX = os.environ.get("PREFIX", "/home/corey/Documents/ghoti.io/.local")
BUILD = os.environ.get("BUILD", "linux/release")
MAKE_BUILD = BUILD.removeprefix("linux/")
APPS = ROOT / "build" / BUILD / "apps"
UNITS = [APPS / "testRaster", APPS / "testGolden", APPS / "testFlatten"]
OBJECT = ROOT / "build" / BUILD / "objects" / "raster" / "raster.o"
SOURCE = "src/raster/raster.c"
LD_PATH = f"{PREFIX}/lib/ghoti.io:{PREFIX}/lib"

# (needle, replacement, what it breaks)
MUTANTS = [
    ("  out.y = (raster->top * GFNT_ONE_PIXEL) - (point.y + raster->origin_y);",
     "  out.y = (raster->top * GFNT_ONE_PIXEL) + (point.y + raster->origin_y);",
     "the y flip"),
    ("  out.y = (raster->top * GFNT_ONE_PIXEL) - (point.y + raster->origin_y);",
     "  out.y = (raster->top * GFNT_ONE_PIXEL) - point.y;",
     "the vertical sub-pixel origin"),
    ("  out.x = point.x + raster->origin_x - raster->left * GFNT_ONE_PIXEL;",
     "  out.x = point.x - raster->left * GFNT_ONE_PIXEL;",
     "the horizontal sub-pixel origin"),
    ("  out.x = point.x + raster->origin_x - raster->left * GFNT_ONE_PIXEL;",
     "  out.x = point.x + raster->origin_x;",
     "the bitmap's left corner"),
    ("  out.y = (raster->top * GFNT_ONE_PIXEL) - (point.y + raster->origin_y);",
     "  out.y = - (point.y + raster->origin_y);",
     "the bitmap's top corner"),
    ("    out->left = raster->left + from->left;", "    out->left = raster->left - from->left;",
     "the view's left"),
    ("    out->top = raster->top - from->top;", "    out->top = raster->top + from->top;",
     "the view's top (y-down back to y-up)"),
    ("    out->top = raster->top - from->top;", "    out->top = from->top;",
     "the view's top ignores the bitmap"),
    ("    out->left = 0;\n    out->top = 0;\n  }", "    out->left = raster->left;\n    out->top = 0;\n  }",
     "an empty result's left"),
    ("    out->left = 0;\n    out->top = 0;\n  }", "    out->left = 0;\n    out->top = raster->top;\n  }",
     "an empty result's top"),
    ("  out->data = from->data;", "  out->data = NULL;", "the pixels are taken"),
    ("  out->width = from->width;\n  out->height = from->height;",
     "  out->width = from->height;\n  out->height = from->width;", "the view's size"),
    ("        gshcpu_coverage_trim(&computed);\n", "", "the trim"),
    ("        options->fill, 0, 0, raster.width, raster.height,",
     "        GFNT_FILL_NONZERO, 0, 0, raster.width, raster.height,", "the fill rule reaches shape-cpu"),
    ("        options->fill, 0, 0, raster.width, raster.height,",
     "        options->fill, 0, 0, raster.width - 1, raster.height,", "the rectangle's width"),
    ("        options->fill, 0, 0, raster.width, raster.height,",
     "        options->fill, 0, 0, raster.width, raster.height - 1,", "the rectangle's height"),
    ("  cpu_caps.max_dimension = caps.max_ppem;", "  cpu_caps.max_dimension = 1;", "the dimension cap"),
    ("  cpu_caps.max_cells = SIZE_MAX;", "  cpu_caps.max_cells = 1;", "the cell cap is lifted"),
    ("  cpu_caps.max_points = SIZE_MAX;", "  cpu_caps.max_points = 1;", "the point cap is lifted"),
    ("  path_caps.max_elements = SIZE_MAX;", "  path_caps.max_elements = 1;", "the element cap is lifted"),
    ("  gfnt_raster_check(raster, gshgeom_device_path_move_to(raster->path, point));\n  return raster->result;",
     "  (void)point;\n  return raster->result;", "the move"),
    ("  raster->contour_start = placed;\n  raster->current = placed;\n",
     "  raster->contour_start = placed;\n", "the pen at a move"),
    ("  point.y = to.y;\n  raster->current = to;\n", "  point.y = to.y;\n", "the pen at a vertex"),
    ("  (void)gfnt_raster_line(raster, placed);\n  return raster->result;\n}\n\nstatic GFNT_Result gfnt_raster_quad_to",
     "  (void)placed;\n  return raster->result;\n}\n\nstatic GFNT_Result gfnt_raster_quad_to", "a line"),
    ("      gfnt_raster_device(gfnt_raster_place(raster, control)),\n      gfnt_raster_device(gfnt_raster_place(raster, to)),",
     "      gfnt_raster_device(gfnt_raster_place(raster, to)),\n      gfnt_raster_device(gfnt_raster_place(raster, control)),",
     "a quadratic's control and end swapped"),
    ("      gfnt_raster_device(gfnt_raster_place(raster, c1)),\n      gfnt_raster_device(gfnt_raster_place(raster, c2)),",
     "      gfnt_raster_device(gfnt_raster_place(raster, c2)),\n      gfnt_raster_device(gfnt_raster_place(raster, c1)),",
     "a cubic's controls swapped"),
    ("  gfnt_raster_check(raster, gshgeom_flatten_quadratic(raster->tolerance,",
     "  gfnt_raster_check(raster, gshgeom_flatten_quadratic(GSHGEOM_FLATTEN_TOLERANCE,",
     "the tolerance reaches the quadratic flattener"),
    ("  gfnt_raster_check(raster, gshgeom_flatten_cubic(raster->tolerance,",
     "  gfnt_raster_check(raster, gshgeom_flatten_cubic(GSHGEOM_FLATTEN_TOLERANCE,",
     "the tolerance reaches the cubic flattener"),
    ("      gfnt_raster_device(raster->current),\n      gfnt_raster_device(gfnt_raster_place(raster, control)),",
     "      gfnt_raster_device(raster->contour_start),\n      gfnt_raster_device(gfnt_raster_place(raster, control)),",
     "a quadratic starts where the contour did"),
    ("  raster.tolerance = options->tolerance ? options->tolerance\n                                        : GFNT_RASTER_TOLERANCE;",
     "  raster.tolerance = GFNT_RASTER_TOLERANCE;", "the tolerance option is read"),
    ("#define GFNT_RASTER_TOLERANCE (GFNT_ONE_PIXEL / 16)", "#define GFNT_RASTER_TOLERANCE (GFNT_ONE_PIXEL / 8)",
     "the default tolerance"),
    ("  if (options->tolerance < 0\n      || (options->tolerance != 0\n          && options->tolerance < GFNT_RASTER_MIN_TOLERANCE)) {",
     "  if (options->tolerance < 0\n      || (options->tolerance != 0\n          && options->tolerance <= GFNT_RASTER_MIN_TOLERANCE)) {",
     "the smallest tolerance is refused"),
    ("  if (options->tolerance < 0\n      || (options->tolerance != 0\n          && options->tolerance < GFNT_RASTER_MIN_TOLERANCE)) {",
     "  if (options->tolerance < 0\n      || (options->tolerance != 0\n          && options->tolerance < GFNT_RASTER_MIN_TOLERANCE - 1)) {",
     "a tolerance of one is accepted"),
    ("  if (options->tolerance < 0\n      || (options->tolerance != 0\n          && options->tolerance < GFNT_RASTER_MIN_TOLERANCE)) {",
     "  if (options->tolerance < GFNT_RASTER_MIN_TOLERANCE) {",
     "the default tolerance (zero) is refused"),
    ("      case GSHCPU_ERR_OOM:\n        result = gfnt_error_set(error, GFNT_ERR_OOM,",
     "      case GSHCPU_ERR_OOM:\n        result = gfnt_error_set(error, GFNT_ERR_INTERNAL,", "shape-cpu's out of memory"),
    ("    case GSHGEOM_ERR_OOM:\n      raster->result = gfnt_error_set(raster->error, GFNT_ERR_OOM,",
     "    case GSHGEOM_ERR_OOM:\n      raster->result = gfnt_error_set(raster->error, GFNT_ERR_INTERNAL,", "the device path's out of memory"),
    ("  gshgeom_device_path_destroy(raster.path);\n  if (result != GFNT_OK) {", "  if (result != GFNT_OK) {", "the device path is released"),
    ("        &cpu_caps, allocator, &computed);", "        &cpu_caps, NULL, &computed);", "shape-cpu allocates with the caller's allocator"),
    ("  if (gshgeom_device_path_create(allocator, &path_caps, &raster.path)",
     "  if (gshgeom_device_path_create(NULL, &path_caps, &raster.path)", "the device path allocates with the caller's allocator"),
    ("  return gshgeom_fill_rule_string(rule);", "  (void)rule;\n  return \"fill\";", "the fill rule's name"),
]


def run(command, *, env=None, expect_failure=False):
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                          env={**os.environ, "LD_LIBRARY_PATH": LD_PATH, **(env or {})})
    if (done.returncode != 0) != expect_failure:
        want = "failure" if expect_failure else "success"
        raise RuntimeError(f"expected {want} from {' '.join(map(str, command))} "
                           f"(exit {done.returncode}):\n{(done.stdout + done.stderr)[-600:]}")


def rebuild():
    for app in UNITS:
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", str(app.relative_to(ROOT))])


def forget():
    for artifact in [OBJECT, *UNITS, *map(Path, glob.glob(str(APPS / "libghoti.io-font-*.a")))]:
        artifact.unlink(missing_ok=True)


def main():
    path = ROOT / SOURCE
    original = path.read_text()
    try:
        run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}"] + [str(a.relative_to(ROOT)) for a in UNITS])
        for unit in UNITS:   # the control: the unmutated build passes
            run([str(unit), "--gtest_brief=1"])
        for needle, replacement, what in MUTANTS:
            if original.count(needle) != 1:
                raise RuntimeError(f"{SOURCE}: the needle for '{what}' matches "
                                   f"{original.count(needle)} times, not once")
            if needle == replacement:
                raise RuntimeError(f"'{what}' changes nothing")
            try:
                path.write_text(original.replace(needle, replacement))
                forget()
                try:
                    rebuild()
                except RuntimeError as error:
                    raise RuntimeError(f"the mutant '{what}' does not build:\n{error}")
                caught = []
                for unit in UNITS:
                    done = subprocess.run([str(unit), "--gtest_brief=1"], cwd=ROOT,
                                          capture_output=True, text=True,
                                          env={**os.environ, "LD_LIBRARY_PATH": LD_PATH})
                    if done.returncode != 0:
                        caught.append(unit.name)
                if not caught:
                    raise RuntimeError(f"NOT CAUGHT: {what}")
            finally:
                path.write_text(original)
                forget()
            print(f"raster_mutation_check: rejected by {', '.join(caught)}: {what}")
        rebuild()
    except (OSError, RuntimeError) as error:
        path.write_text(original)
        print(f"raster_mutation_check: {error}", file=sys.stderr)
        return 1
    print(f"raster_mutation_check: {len(MUTANTS)} mutants, all rejected, and the control passes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
