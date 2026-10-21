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
"""Plant a defect in the COLR lowering and see the tests or the differential reject it.

Story 14c lowers a glyph's `COLR` into a `shape-scene` list (src/scene/colr.c,
scene_color.h) and interns a `CPAL` palette. Each mutant below is one literal
replacement in colr.c, scene.c's binding accessor or scene_color.h; it must
match exactly once and be refused by testScene_color, or by the differential
(tools/oracle/colr_scene_diff.py's comparison of `font-colr-scene` against the
fontTools lowering, computed once before the mutants, over the color fixtures
and the image's color fonts: the only population with every composite mode and
every transform at real values), or, for a planted floating type, by `make
check-float-free`. The file is restored and the artifacts rebuilt after every
mutant.

What is NOT planted, and why:

  - the walk's node bound (GFNT_PAINT_WALK_NODES): no committed or corpus font
    reaches it before the depth bound, which a graph that names itself reaches
  - the allocation failure of a palette's entries and a failing palette read: a
    face that loaded has a CPAL the reader validated, and the allocator does not
    fail here
  - the palette index's bound in gfnt_scene_intern_palette: `gfnt_face_palette_color`
    refuses the same index with the same result, for a palette with any entry
  - the truncation's own failure: `gshscene_list_truncate` refuses only a closed
    list or a count past the list's, and the lowering reads the count of the open
    list it records into

Usage: colr_mutation_check.py   (PREFIX and BUILD as for make)
"""

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "oracle"))
PREFIX = os.environ.get("PREFIX", str(ROOT.parents[1] / ".local"))
BUILD = os.environ.get("BUILD", "linux/release")
MAKE_BUILD = BUILD.removeprefix("linux/")
APPS = ROOT / "build" / BUILD / "apps"
OBJECTS = ROOT / "build" / BUILD / "objects"
LD_PATH = f"{PREFIX}/lib/ghoti.io"
UNITS = [APPS / "testScene_color"]
DRIVER = APPS / "examples" / "font-colr-scene"
FLOAT = "float"

COLR = "src/scene/colr.c"
SCENE = "src/scene/scene.c"
HEADER = "include/ghoti.io/font/scene_color.h"

MUTANTS = [
    # The operators of COLR's modes.
    (COLR, "  1u,   // clear", "  0u,   // clear", "clear as source-over", None),
    (COLR, "  2u,   // src", "  3u,   // src", "src as dest", None),
    (COLR, "  3u,   // dest", "  2u,   // dest", "dest as src", None),
    (COLR, "  0u,   // src over", "  1u,   // src over", "source-over as clear", None),
    (COLR, "  5u,   // src in", "  6u,   // src in", "src in as dest in", None),
    (COLR, "  11u,  // xor", "  12u,  // xor", "xor as plus", None),
    (COLR, "  12u,  // plus", "  11u,  // plus", "plus as xor", None),
    (COLR, "  if (p->composite_mode > 12u) {", "  if (p->composite_mode > 13u) {", "screen taken for an operator", None),
    (COLR, "  GFNT_Result r = gfnt_colr_scene(l, gshscene_list_group_push_fixed(l->list, 0u, l->run->layer),",
     "  GFNT_Result r = gfnt_colr_scene(l, gshscene_list_group_push_fixed(l->list, 1u, l->run->layer),",
     "the composite's outer group not source-over", None),
    (COLR, "    r = gfnt_colr_node(l, p->backdrop, depth + 1);\n  }\n  if (r == GFNT_OK) {\n    r = gfnt_colr_scene",
     "    r = gfnt_colr_node(l, p->child, depth + 1);\n  }\n  if (r == GFNT_OK) {\n    r = gfnt_colr_scene",
     "the source drawn as the backdrop", None),
    (COLR, "        \"a blend mode: the scene composites the Porter-Duff operators and plus only\");",
     "        \"a blend mode: the scene composites the Porter-Duff operators and plus only\") * 0;",
     "a blend mode lowered as nothing", None),
    (COLR, "    l->out->composite_mode = p->composite_mode;\n", "", "a refused mode not named", None),
    # The paint.
    (COLR, "  p.alpha = alpha < 0 ? 0 : alpha > GFNT_COLR_ONE ? GFNT_COLR_ONE : alpha;",
     "  p.alpha = alpha < 0 ? 0 : alpha;", "an alpha past one not clamped", None),
    (COLR, "  p.bits = 14u;", "  p.bits = 15u;", "the alpha's fraction bits", None),
    (COLR, "  p.entry = entry;", "  p.entry = entry == 0xFFFFu ? 0u : entry;", "the foreground as entry 0", None),
    (COLR, "      GSHSCENE_Paint_Fixed paint = gfnt_colr_paint(layer.palette_index, GFNT_COLR_ONE);",
     "      GSHSCENE_Paint_Fixed paint = gfnt_colr_paint(layer.palette_index, GFNT_COLR_ONE / 2);",
     "a version 0 layer at half alpha", None),
    # The shapes.
    (COLR, "  r = gfnt_colr_scene(l, gshscene_list_fill_fixed(l->list, shape, l->op_count ? l->ops : NULL,\n      l->op_count,",
     "  r = gfnt_colr_scene(l, gshscene_list_fill_fixed(l->list, shape, NULL,\n      0,",
     "a fill without its transforms", None),
    (COLR, "      l->op_count, GSHGEOM_FILL_NONZERO, paint, l->run->layer), \"the scene refused a fill\");",
     "      l->op_count, GSHGEOM_FILL_NONZERO, paint, 0), \"the scene refused a fill\");", "a fill's layer", None),
    (COLR, "  g.x = l->run->x;", "  g.x = 0;", "the run's origin", None),
    (COLR, "      l->run->size, l->run->hint_mode, l->run->hint_version, l->run->bits, 1, &g, out),",
     "      l->run->size, GSHSCENE_HINT_FONT, l->run->hint_version, l->run->bits, 1, &g, out),",
     "the run's hint mode", None),
    (COLR, "  r = gfnt_colr_scene(l, gshscene_list_clip_push_fixed(l->list, shape, l->op_count ? l->ops : NULL,\n      l->op_count,",
     "  r = gfnt_colr_scene(l, gshscene_list_clip_push_fixed(l->list, shape, NULL,\n      0,",
     "a clip without its transforms", None),
    (COLR, "    r = gfnt_colr_scene(l, gshscene_list_clip_pop(l->list), \"the scene refused a clip's pop\");",
     "    r = gfnt_colr_scene(l, gshscene_list_clip_pop(l->list), \"the scene refused a clip's pop\");\n"
     "    gshscene_list_clip_push_fixed(l->list, shape, NULL, 0, GSHGEOM_FILL_NONZERO, 0);",
     "a clip left open", None),
    (COLR, "    ref = c.child;\n    ++d;", "    break;", "a solid under a transform not seen through", None),
    (COLR, "  if (c.format == GFNT_PAINT_SOLID) {\n    GSHSCENE_Paint_Fixed paint = gfnt_colr_paint(c.palette_index, c.v[0]);",
     "  if (0) {\n    GSHSCENE_Paint_Fixed paint = gfnt_colr_paint(c.palette_index, c.v[0]);",
     "a glyph over a solid as a clip", None),
    # The transforms.
    (COLR, "      op->kind = GSHSCENE_FIXED_AFFINE;\n      op->bits = 16u;", "      op->kind = GSHSCENE_FIXED_AFFINE;\n      op->bits = 14u;",
     "Fixed read at 14 bits", None),
    (COLR, "      op->kind = GSHSCENE_FIXED_TRANSLATE;\n      op->v[0] = p->v[0];\n      op->v[1] = p->v[1];",
     "      op->kind = GSHSCENE_FIXED_TRANSLATE;\n      op->v[0] = p->v[1];\n      op->v[1] = p->v[0];",
     "a translation's axes", None),
    (COLR, "        op->v[2] = p->v[2] * 16384;\n        op->v[3] = p->v[3] * 16384;",
     "        op->v[2] = p->v[2];\n        op->v[3] = p->v[3] * 16384;", "a scale's center at the wrong bits", None),
    (COLR, "      op->v[1] = p->v[0];\n      if (p->format == GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER) {",
     "      op->v[1] = 16384;\n      if (p->format == GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER) {", "a uniform scale in x only", None),
    (COLR, "        op->v[2] = p->v[1] * 16384;\n        op->v[3] = p->v[2] * 16384;",
     "        op->v[2] = p->v[2] * 16384;\n        op->v[3] = p->v[1] * 16384;", "a uniform scale's center", None),
    (COLR, "      op->kind = GSHSCENE_FIXED_ROTATE;\n      op->bits = 15u;", "      op->kind = GSHSCENE_FIXED_ROTATE;\n      op->bits = 14u;",
     "an angle in turns rather than half turns", None),
    (COLR, "        op->v[1] = p->v[1] * 32768;\n        op->v[2] = p->v[2] * 32768;",
     "        op->v[1] = p->v[1] * 16384;\n        op->v[2] = p->v[2] * 32768;", "a rotation's center", None),
    (COLR, "      op->kind = GSHSCENE_FIXED_SKEW;\n      op->bits = 15u;\n      op->v[0] = p->v[0];\n      op->v[1] = p->v[1];",
     "      op->kind = GSHSCENE_FIXED_SKEW;\n      op->bits = 15u;\n      op->v[0] = p->v[1];\n      op->v[1] = p->v[0];",
     "a skew's angles", None),
    (COLR, "        op->v[2] = p->v[2] * 32768;\n        op->v[3] = p->v[3] * 32768;",
     "        op->v[2] = p->v[3] * 32768;\n        op->v[3] = p->v[2] * 32768;", "a skew's center", None),
    (COLR, "  l->ops[l->op_count++] = op;", "  l->ops[l->op_count] = op;", "a transform dropped", None),
    (COLR, "  r = gfnt_colr_node(l, p.child, depth + 1);\n  l->op_count--;", "  r = gfnt_colr_node(l, p.child, depth + 1);",
     "a transform kept past its subtree", None),
    # The walk.
    (COLR, "  if (depth > l->max_depth || ++l->nodes > GFNT_PAINT_WALK_NODES) {",
     "  if (depth > l->max_depth * 64u || ++l->nodes > GFNT_PAINT_WALK_NODES) {", "a depth bound 64 times too deep", None),
    (COLR, "  if (r == GFNT_OK && p->variable) {", "  if (0) {", "a variable paint lowered at its default", None),
    (COLR, "      for (uint32_t i = 0; i < p.layer_count && r == GFNT_OK; ++i) {",
     "      for (uint32_t i = 1; i < p.layer_count && r == GFNT_OK; ++i) {", "a layer list's first layer skipped", None),
    (COLR, "      if (r == GFNT_OK && root == 0) {\n        return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_NO_RECORD,",
     "      if (0) {\n        return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_NO_RECORD,", "a color glyph with no record", None),
    (COLR, "      return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_GRADIENT, (uint32_t)p.format,",
     "      return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_UNBOUNDED, (uint32_t)p.format,", "a gradient's reason", None),
    (COLR, "    case GFNT_PAINT_SOLID:\n      return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_UNBOUNDED, (uint32_t)p.format,",
     "    case GFNT_PAINT_SOLID:\n      return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_GRADIENT, (uint32_t)p.format,",
     "an unbounded solid's reason", None),
    (COLR, "  l->out->paint_format = format;", "  (void)format;\n  l->out->paint_format = 0;", "a refusal's format", None),
    # The call.
    (COLR, "  out->source = root ? GFNT_COLOR_SOURCE_V1 : GFNT_COLOR_SOURCE_V0;",
     "  out->source = GFNT_COLOR_SOURCE_V1;", "the source", None),
    (COLR, "  if (r == GFNT_OK && root == 0) {\n    r = gfnt_face_color_layer_count(l.face, glyph, &layers, error);\n  }",
     "  if (r == GFNT_OK) {\n    r = gfnt_face_color_layer_count(l.face, glyph, &layers, error);\n    root = layers ? 0 : root;\n  }",
     "version 0 preferred to version 1", None),
    (COLR, "  r = root ? gfnt_colr_node(&l, root, 0) : gfnt_colr_layers(&l, layers);",
     "  r = root ? gfnt_colr_layers(&l, layers) : gfnt_colr_node(&l, root, 0);",
     "each version lowered by the other's walk", None),
    (COLR, "    if (gshscene_list_truncate(list, before) != GSHSCENE_OK) {", "    if (0) {", "a refusal leaves what it recorded", None),
    (COLR, "  out->items = gshscene_list_count(list) - before;", "  out->items = gshscene_list_count(list);",
     "the items counted from zero", None),
    (COLR, "  l.face = gfnt_scene_face_of(face);\n  if (!l.face) {", "  l.face = gfnt_scene_face_of(face);\n  if (0) {",
     "an unbound face lowered", None),
    # The palette.
    (COLR, "      rgba[4u * i] = c.red;\n      rgba[4u * i + 1u] = c.green;",
     "      rgba[4u * i] = c.blue;\n      rgba[4u * i + 1u] = c.green;", "red read as blue", None),
    (COLR, "      rgba[4u * i + 3u] = c.alpha;", "      rgba[4u * i + 3u] = 255;", "an entry's alpha", None),
    (COLR, "    r = gfnt_face_palette_color(f, palette, i, &c, error);", "    r = gfnt_face_palette_color(f, 0, i, &c, error);",
     "every palette read as the first", None),
    # The binding.
    (SCENE, "      || ops->glyph_outline != gfnt_scene_ops.glyph_outline || !ctx) {", "      || !ctx) {",
     "another provider's binding taken for this library's", None),
    # The float-free gate.
    (COLR, "/** F2.14's one: a solid's alpha is clamped to `[0, GFNT_COLR_ONE]`. */",
     "typedef double gfnt_planted_t;\n/** F2.14's one: a solid's alpha is clamped to `[0, GFNT_COLR_ONE]`. */",
     "a double in the lowering", FLOAT),
    (HEADER, "/** @brief Which description a lowering used. */",
     "typedef float gfnt_planted_t;\n/** @brief Which description a lowering used. */", "a float in scene_color.h", FLOAT),
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
    make(*[str(u.relative_to(ROOT)) for u in UNITS], str(DRIVER.relative_to(ROOT)))


def forget(source):
    obj = OBJECTS / Path(source).relative_to("src").with_suffix(".o") if source.startswith("src/") else None
    for artifact in [*([obj] if obj else []), *UNITS, DRIVER, *APPS.glob("libghoti.io-font-*.a")]:
        artifact.unlink(missing_ok=True)


def driver_answers(fonts):
    out = {}
    for font in fonts:
        done = subprocess.run([str(DRIVER), font], cwd=ROOT, capture_output=True, text=True,
                              env={**os.environ, "LD_LIBRARY_PATH": LD_PATH})
        try:
            out[font] = json.loads(done.stdout)["glyphs"]
        except (ValueError, KeyError):
            out[font] = {"*": {"error": "no answer"}}
    return out


def differs(ours, want):
    import colr_scene_diff
    for font, got in ours.items():
        ref = want[font]["glyphs"]
        if set(got) != set(ref):
            return True
        for gid in got:
            if colr_scene_diff.differ(got[gid], ref[gid]):
                return True
    return False


def reference():
    """The fontTools lowering of every color font, once (it does not depend on the mutants)."""
    import colr_scene_diff
    import corpus
    import oracle_env
    fixtures = [str(ROOT / "tests" / "data" / "fonts" / n) for n in sorted(os.listdir(ROOT / "tests" / "data" / "fonts"))
                if n.endswith((".ttf", ".otf")) and colr_scene_diff.has_colr(str(ROOT / "tests" / "data" / "fonts" / n))]
    fonts = fixtures + [f for f in corpus.fonts("sfnt") if colr_scene_diff.has_colr(f)]
    scratch = ROOT / "build" / "oracle" / "colr-scene"
    scratch.mkdir(parents=True, exist_ok=True)
    out = scratch / "fonttools-mutations.json"
    argv = oracle_env.command("fonttools", ["python3", str(ROOT / "tools" / "oracle" / "fonttools_colr_scene.py"),
                                            str(out)] + fonts, scratch=str(scratch))
    done = subprocess.run(argv, capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError("the fontTools lowering failed: " + done.stderr[-400:])
    return fonts, json.loads(out.read_text())


def main():
    originals = {f: (ROOT / f).read_text() for f in {m[0] for m in MUTANTS}}
    caught_all = 0
    missed = []
    try:
        fonts, want = reference()
        rebuild()
        for unit in UNITS:   # the control: the unmutated build passes, and agrees with fontTools
            run([str(unit), "--gtest_brief=1"])
        if differs(driver_answers(fonts), want):
            raise RuntimeError("the control differs from the fontTools lowering")
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
                    if differs(driver_answers(fonts), want):
                        caught.append("colr_scene_diff")
            finally:
                path.write_text(original)
                forget(source)
            if not caught:
                missed.append(what)
                print(f"colr_mutation_check: NOT CAUGHT: {what}", flush=True)
                continue
            caught_all += 1
            print(f"colr_mutation_check: rejected by {', '.join(caught)}: {what}", flush=True)
        rebuild()
        if missed:
            raise RuntimeError(f"{len(missed)} mutants not caught: " + "; ".join(missed))
    except (OSError, RuntimeError) as error:
        for f, text in originals.items():
            (ROOT / f).write_text(text)
        print(f"colr_mutation_check: {error}", file=sys.stderr)
        return 1
    print(f"colr_mutation_check: {caught_all} of {len(MUTANTS)} mutants rejected, and the control passes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
