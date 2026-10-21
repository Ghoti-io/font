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
"""Plant a defect in the glyph path provider and see the gates reject it.

Story 14b of the shape family's first milestone made this library shape-scene's
glyph path provider (src/scene/scene.c), moved its unhinted arithmetic to the one
rounding (src/scene/unhinted.c, D1), and sent gfnt_face_render_glyph's unhinted
cases through it (src/raster/raster.c). Its gates are AD-5's (the fast tier is
the general route under the snapped map, and is taken), AD-1's (the direct call
is a one-glyph scene), the pinned ties, and the float-free headers. A gate that
has only ever passed has not been shown to be able to fail, so each mutant below
is one literal replacement in one of those files; it must match exactly once,
and must be refused by a unit test (testScene, testUnhinted, testGolden,
testRaster) or, for a planted floating type, by `make check-float-free`. The
file is restored and the artifacts rebuilt after every mutant, whatever
happened.

What is NOT planted, and why:

  - the hint-context cache's lock: a missing lock is a race, which no test here
    can make happen on demand (the threads test calls the ops at once and checks
    the answers, and is the place a sanitizer build would see one)
  - the failure arms of a mutex lock and of an allocation in scene.c: they are
    unreachable without a failing lock or allocator, and every one returns a
    failure the caller already handles
  - `gfnt_scene_result`'s and `gfnt_scene_from`'s arms for IO and INTERNAL: this
    library returns neither from an outline or a store intern
  - the race arm of the cache (another call made the same context meanwhile):
    reached only by two calls missing at once, which the threads test may or may
    not do; its effect is a context made and freed, which no answer shows

Usage: scene_mutation_check.py   (PREFIX and BUILD as for make)
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
UNITS = [APPS / "testScene", APPS / "testUnhinted", APPS / "testGolden", APPS / "testRaster"]
FLOAT = "float"   # a mutant checked by `make check-float-free` rather than by the units

UNHINTED = "src/scene/unhinted.c"
SCENE = "src/scene/scene.c"
RASTER = "src/raster/raster.c"
HEADER = "include/ghoti.io/font/scene.h"

MUTANTS = [
    # The one rounding (D1).
    (UNHINTED, "    r = gshgeom_scale_26_6(point.y, walk->scale, 1, &y);",
     "    r = gshgeom_scale_26_6(point.y, walk->scale, 0, &y);\n    y = -y;",
     "y rounded half up in y up rather than in y down", None),
    (UNHINTED, "  ax = (int64_t)walk->fx + x;", "  ax = x;", "the fraction in x", None),
    (UNHINTED, "  ay = (int64_t)walk->fy + y;", "  ay = y;", "the fraction in y", None),
    (UNHINTED, "  point.y = -p.y;", "  point.y = p.y;", "the copy's y turned back up", None),
    (UNHINTED, "  return r != GFNT_OK ? r : walk->sink->quad_to(walk->user, c, p);",
     "  return r != GFNT_OK ? r : walk->sink->quad_to(walk->user, p, c);", "a quadratic's points", None),
    (UNHINTED, "  return r != GFNT_OK ? r : walk->sink->cubic_to(walk->user, a, b, p);",
     "  return r != GFNT_OK ? r : walk->sink->cubic_to(walk->user, b, a, p);", "a cubic's controls", None),
    (UNHINTED, "  result = gfnt_unhinted_pixels(units, gfnt_scale_for_ppem(upem, ppem),",
     "  result = gfnt_unhinted_pixels(units, gfnt_scale_for_ppem(upem, ppem) + 1,",
     "the renderer's scale", None),
    (UNHINTED, "  caps.max_outline_points = SIZE_MAX;", "  caps.max_outline_points = caps.max_outline_points;",
     "the copy held to the outline's point cap", None),
    (UNHINTED, "  if (gfnt_outline_space(units) != GFNT_OUTLINE_UNITS) {", "  if (0) {",
     "the walk takes an outline in pixels", None),
    # The renderer's use of it (AD-1).
    (RASTER, "        result = gfnt_unhinted_render(face, glyph, upem, ppem, options,\n"
             "            allocator, out_coverage, error);",
     "        result = gfnt_raster_outline(outline, options, &face->limits, allocator,\n"
     "            out_coverage, error);",
     "a glyph the hinter returns unhinted is drawn as the hinter scaled it", None),
    (RASTER, "  if (result == GFNT_OK && (status == GFNT_HINT_STATUS_HINTED\n"
             "          || status == GFNT_HINT_STATUS_HINTED_WITH_ERRORS)) {",
     "  if (result == GFNT_OK && (status == GFNT_HINT_STATUS_HINTED)) {",
     "a glyph hinted with errors is drawn unhinted", None),
    (RASTER, "  return gfnt_unhinted_render(face, glyph, upem, ppem, options, allocator,\n"
             "      out_coverage, error);\n}",
     "  {\n    GFNT_Outline * units = NULL;\n"
     "    result = gfnt_face_glyph_outline(face, glyph, options ? options->variation : NULL,\n"
     "        allocator, &units, error);\n"
     "    if (result == GFNT_OK) result = gfnt_outline_scale(units, gfnt_scale_for_ppem(upem, ppem), error);\n"
     "    if (result == GFNT_OK) result = gfnt_raster_outline(units, options, &face->limits, allocator,\n"
     "        out_coverage, error);\n"
     "    gfnt_outline_destroy(units);\n    return result;\n  }\n}",
     "the previous rounding (scale, then decompose)", None),
    # The provider.
    (SCENE, "  gfnt_scene_glyph_device_path,\n  gfnt_scene_glyph_box,", "  NULL,\n  gfnt_scene_glyph_box,",
     "always general: no fast tier", None),
    (SCENE, "? GSHSCENE_HINT_STATUS_HINTED : GSHSCENE_HINT_STATUS_WITH_ERRORS;",
     "? GSHSCENE_HINT_STATUS_WITH_ERRORS : GSHSCENE_HINT_STATUS_HINTED;", "the hinted statuses swapped", None),
    (SCENE, "        if (rq->hint_mode == GSHSCENE_HINT_FONT_STRICT) {\n          return GSHSCENE_ERR_REFUSED;",
     "        if (0) {\n          return GSHSCENE_ERR_REFUSED;", "a failing program under strict is drawn", None),
    (SCENE, "    if (s->refs == 0 && (i < 0 || s->used < b->slots[i].used)) {",
     "    if (s->refs == 0 && (i < 0 || s->used > b->slots[i].used)) {", "the cache drops the most recent", None),
    (SCENE, "  if (i < 0) {\n    // Every entry is in use", "  if (1) {\n    // Every entry is in use",
     "the cache keeps nothing", None),
    (SCENE, "      gfnt_hint_context_destroy(b->slots[i].context);\n"
            "      gcu_atomic_size_fetch_sub(&font->contexts_live, 1);",
     "      (void)0;", "the last release keeps the cache", None),
    (SCENE, "  return gfnt_face_glyph_outline(b->font->face, glyph, &variation,",
     "  (void)variation;\n  return gfnt_face_glyph_outline(b->font->face, glyph, NULL,", "the instance ignored", None),
    (SCENE, "  out->x_max = box.x_max;", "  out->x_max = box.x_min;", "the box", None),
    (SCENE, "  font->key = hash ? hash : 1u;", "  font->key = 1u;", "the key is not the bytes'", None),
    (SCENE, "  out->family_name = font->family;", "  out->family_name = font->postscript;", "R1's names", None),
    (SCENE, "  return gfnt_unhinted_geometry_error(gshgeom_path_move_to_fixed(user, to.x,\n"
            "      to.y, GSHSCENE_GLYPH_OUTLINE_BITS), NULL);",
     "  return gfnt_unhinted_geometry_error(gshgeom_path_move_to_fixed(user, to.x,\n"
     "      to.y, GSHSCENE_GLYPH_OUTLINE_BITS - 1), NULL);", "the outline's fraction bits", None),
    (SCENE, "  const int64_t y = (int64_t)h->fy - p.y;", "  const int64_t y = (int64_t)h->fy + p.y;",
     "a hinted point's y not turned down", None),
    (SCENE, "  h.fx = (int32_t)rq->frac_x;", "  h.fx = 0;", "a hinted glyph's fraction", None),
    (SCENE, "      || scale != rq->scale) {", "      ) {", "a request's scale not checked", None),
    (SCENE, "      && gfnt_face_gasp_gridfit(font->face, rq->ppem)) {", "      ) {", "gasp ignored", None),
    (SCENE, "  return ((uint32_t)maxp[26] << 8 | maxp[27]) != 0;", "  return false;",
     "glyph programs not seen in maxp", None),
    (SCENE, "    if (variation->coords[i] < -GSHSCENE_F2DOT14_ONE\n", "    if (0\n",
     "a coordinate past one", None),
    (SCENE, "  // already held keeps its own, and then this one goes here.\n  gfnt_scene_binding_release(b);",
     "  // already held keeps its own, and then this one goes here.", "this call's reference kept", None),
    (SCENE, "      *advance = scaled;", "      *advance = 0;", "the unhinted advance", None),
    # The float-free gate.
    (UNHINTED, "/** The walk's state: the rounding's parameters and where the points go. */",
     "typedef double gfnt_planted_t;\n/** The walk's state: the rounding's parameters and where the points go. */",
     "a double in the outline-to-pixels code", FLOAT),
    (HEADER, "#define GFNT_SCENE_HINT_CONTEXTS 8u", "#define GFNT_SCENE_HINT_CONTEXTS 8u\ntypedef float gfnt_planted_t;",
     "a float in a GFNT_ header", FLOAT),
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
    obj = OBJECTS / Path(source).relative_to("src").with_suffix(".o") if source.startswith("src/") else None
    for artifact in [*([obj] if obj else []), *UNITS, *map(Path, glob.glob(str(APPS / "libghoti.io-font-*.a")))]:
        artifact.unlink(missing_ok=True)


def main():
    originals = {f: (ROOT / f).read_text() for f in {m[0] for m in MUTANTS}}
    caught_all = 0
    try:
        rebuild()
        for unit in UNITS:   # the control: the unmutated build passes
            run([str(unit), "--gtest_brief=1"])
        make("check-float-free")
        for source, needle, replacement, what, kind in MUTANTS:
            original = originals[source]
            if original.count(needle) != 1:
                raise RuntimeError(f"{source}: the needle for '{what}' matches {original.count(needle)} times, not once")
            if needle == replacement:
                raise RuntimeError(f"'{what}' changes nothing")
            path = ROOT / source
            try:
                path.write_text(original.replace(needle, replacement))
                caught = []
                if kind == FLOAT:
                    done = subprocess.run(["make", f"PREFIX={PREFIX}", f"BUILD={MAKE_BUILD}", "check-float-free"],
                                          cwd=ROOT, capture_output=True, text=True)
                    if done.returncode != 0:
                        caught.append("check-float-free")
                else:
                    forget(source)
                    try:
                        rebuild()
                    except RuntimeError as error:
                        raise RuntimeError(f"the mutant '{what}' does not build:\n{error}")
                    for unit in UNITS:
                        done = subprocess.run([str(unit), "--gtest_brief=1"], cwd=ROOT, capture_output=True,
                                              text=True, env={**os.environ, "LD_LIBRARY_PATH": LD_PATH})
                        if done.returncode != 0:
                            caught.append(unit.name)
                if not caught:
                    raise RuntimeError(f"NOT CAUGHT: {what}")
            finally:
                path.write_text(original)
                forget(source)
            caught_all += 1
            print(f"scene_mutation_check: rejected by {', '.join(caught)}: {what}")
        rebuild()
    except (OSError, RuntimeError) as error:
        for f, text in originals.items():
            (ROOT / f).write_text(text)
        print(f"scene_mutation_check: {error}", file=sys.stderr)
        return 1
    print(f"scene_mutation_check: {caught_all} of {len(MUTANTS)} mutants rejected, and the control passes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
