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

Story 14c (and P4, which lowers every paint) lowers a glyph's `COLR` into a `shape-scene` list (src/scene/colr.c,
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
    # L7: 13 to 27 by identity, anything else clear.
    (COLR, "  return mode <= 27u ? mode : GFNT_COLR_UNKNOWN_MODE_OPERATOR;", "  return mode <= 26u ? mode : GFNT_COLR_UNKNOWN_MODE_OPERATOR;",
     "luminosity (27) taken for clear", None),
    (COLR, "  return mode <= 27u ? mode : GFNT_COLR_UNKNOWN_MODE_OPERATOR;", "  return mode <= 28u ? mode : GFNT_COLR_UNKNOWN_MODE_OPERATOR;",
     "mode 28 taken for a blend mode", None),
    (COLR, "  return mode <= 27u ? mode : GFNT_COLR_UNKNOWN_MODE_OPERATOR;", "  return mode <= 27u ? mode : 0u;",
     "an unknown mode taken for source-over", None),
    (COLR, "  return mode <= 27u ? mode : GFNT_COLR_UNKNOWN_MODE_OPERATOR;", "  return mode <= 27u ? mode - 1u : GFNT_COLR_UNKNOWN_MODE_OPERATOR;",
     "the blend modes off by one", None),
    # (A mutant that narrowed the table's range to end before plus (12) was removed: mode 12 then takes the identity
    # branch below, and the table maps 12 to 12, so it is the same program.)
    (COLR, "  GFNT_Result r = gfnt_colr_scene(l, gshscene_list_group_push_fixed(l->list, 0u, l->run->layer),\n      \"the scene refused a composite's group\");",
     "  GFNT_Result r = gfnt_colr_scene(l, gshscene_list_group_push_fixed(l->list, 1u, l->run->layer),\n      \"the scene refused a composite's group\");",
     "the composite's outer group not source-over", None),
    (COLR, "    r = gfnt_colr_node(l, p->backdrop, depth + 1);\n  }\n  if (r == GFNT_OK) {\n    r = gfnt_colr_scene",
     "    r = gfnt_colr_node(l, p->child, depth + 1);\n  }\n  if (r == GFNT_OK) {\n    r = gfnt_colr_scene",
     "the source drawn as the backdrop", None),
    # The paint.
    (COLR, "  p.alpha = alpha < 0 ? 0 : alpha > GFNT_COLR_ONE ? GFNT_COLR_ONE : alpha;",
     "  p.alpha = alpha < 0 ? 0 : alpha;", "an alpha past one not clamped", None),
    (COLR, "  p.bits = 14u;", "  p.bits = 15u;", "the alpha's fraction bits", None),
    (COLR, "  p.entry = entry;", "  p.entry = entry == 0xFFFFu ? 0u : entry;", "the foreground as entry 0", None),
    (COLR, "      GSHSCENE_Paint_Fixed paint = gfnt_colr_paint(layer.palette_index, GFNT_COLR_ONE);",
     "      GSHSCENE_Paint_Fixed paint = gfnt_colr_paint(layer.palette_index, GFNT_COLR_ONE / 2);",
     "a version 0 layer at half alpha", None),
    # The shapes.
    (COLR, "  r = gfnt_colr_scene(l, gshscene_list_fill_fixed(l->list, shape, NULL, 0, GSHGEOM_FILL_NONZERO, paint,\n      l->run->layer), \"the scene refused a fill\");",
     "  r = gfnt_colr_scene(l, gshscene_list_fill_fixed(l->list, shape, NULL, 0, GSHGEOM_FILL_NONZERO, paint,\n      0), \"the scene refused a fill\");",
     "a version 0 fill's layer", None),
    (COLR, "  g.x = l->run->x;", "  g.x = 0;", "the run's origin", None),
    (COLR, "      l->run->size, l->run->hint_mode, l->run->hint_version, l->run->bits, 1, &g, out),",
     "      l->run->size, GSHSCENE_HINT_FONT, l->run->hint_version, l->run->bits, 1, &g, out),",
     "the run's hint mode", None),
    # L4: what fills a glyph, and where its transforms go.
    (COLR, "    r = gfnt_colr_leaf(l, &c, shape, l->ops, l->op_count, between, between_count);",
     "    r = gfnt_colr_leaf(l, &c, shape, l->ops, l->op_count, between, 0);",
     "the transforms between a glyph and its gradient dropped", None),
    (COLR, "    r = gfnt_colr_leaf(l, &c, shape, l->ops, l->op_count, between, between_count);",
     "    r = gfnt_colr_leaf(l, &c, shape, between, between_count, between, between_count);",
     "the lower transforms put on the outline", None),
    (COLR, "    r = gfnt_colr_leaf(l, &c, shape, l->ops, l->op_count, between, between_count);",
     "    r = gfnt_colr_leaf(l, &c, shape, NULL, 0, between, between_count);",
     "a fill without the transforms above its glyph", None),
    (COLR, "  r = gfnt_colr_scene(l, gshscene_list_clip_push_fixed(l->list, shape, l->op_count ? l->ops : NULL,\n      l->op_count,",
     "  r = gfnt_colr_scene(l, gshscene_list_clip_push_fixed(l->list, shape, NULL,\n      0,",
     "a clip without its transforms", None),
    (COLR, "    r = gfnt_colr_scene(l, gshscene_list_clip_pop(l->list), \"the scene refused a clip's pop\");\n  }\n  return r;\n}\n\n/** The scene's operator",
     "    r = gfnt_colr_scene(l, gshscene_list_clip_pop(l->list), \"the scene refused a clip's pop\");\n    gshscene_list_clip_push_fixed(l->list, shape, NULL, 0, GSHGEOM_FILL_NONZERO, 0);\n  }\n  return r;\n}\n\n/** The scene's operator",
     "a clip left open", None),
    (COLR, "    between[between_count++] = op;\n    ref = c.child;\n    ++d;", "    break;", "a leaf under a transform not seen through", None),
    (COLR, "  if (gfnt_colr_is_leaf(c.format)) {\n    r = gfnt_colr_leaf(", "  if (0) {\n    r = gfnt_colr_leaf(", "a glyph over a leaf as a clip", None),
    # L6: the plane and the bound.
    (COLR, "      if (!l->bounded) {\n        return gfnt_colr_refuse(", "      if (0) {\n        return gfnt_colr_refuse(",
     "an unbounded paint drawn with no bound", None),
    (COLR, "    l->bounded = true;\n    l->bound_ops = l->op_count;\n    r = gfnt_colr_node(l, p->child, depth + 1);",
     "    r = gfnt_colr_node(l, p->child, depth + 1);", "a glyph's clip not a bound", None),
    (COLR, "    l->bounded = true;\n    l->bound_ops = l->op_count;\n    r = gfnt_colr_node(l, p->child, depth + 1);",
     "    l->bounded = true;\n    l->bound_ops = 0;\n    r = gfnt_colr_node(l, p->child, depth + 1);", "the bound's transforms forgotten", None),
    (COLR, "    l->bounded = was_bounded;\n    l->bound_ops = was_ops;", "    l->bounded = true;\n    (void)was_bounded;\n    l->bound_ops = was_ops;", "a bound kept past its clip", None),
    (COLR, "      return r != GFNT_OK ? r : gfnt_colr_leaf(l, &p, l->plane, l->ops, l->bound_ops,\n          l->ops + l->bound_ops, l->op_count - l->bound_ops);",
     "      return r != GFNT_OK ? r : gfnt_colr_leaf(l, &p, l->plane, l->ops, l->op_count,\n          l->ops + l->bound_ops, l->op_count - l->bound_ops);",
     "a plane fill's map with every transform", None),
    (COLR, "      return r != GFNT_OK ? r : gfnt_colr_leaf(l, &p, l->plane, l->ops, l->bound_ops,\n          l->ops + l->bound_ops, l->op_count - l->bound_ops);",
     "      return r != GFNT_OK ? r : gfnt_colr_leaf(l, &p, l->plane, l->ops, l->bound_ops,\n          l->ops, l->op_count);",
     "a plane gradient's map with every transform", None),
    (COLR, "      return r != GFNT_OK ? r : gfnt_colr_leaf(l, &p, l->plane, l->ops, l->bound_ops,\n          l->ops + l->bound_ops, l->op_count - l->bound_ops);",
     "      return r != GFNT_OK ? r : gfnt_colr_leaf(l, &p, l->plane, l->ops, l->bound_ops,\n          NULL, 0);",
     "a plane gradient's own transforms dropped", None),
    # L5: the clip box.
    (COLR, "  if (r == GFNT_OK && found) {\n    r = gfnt_colr_scene(l, gshscene_list_clip_push_design_rect_fixed(l->list, l->frame, NULL, 0, box.x_min, box.y_min,\n        box.x_max, box.y_max, 0u, l->run->layer),",
     "  if (r == GFNT_OK && found) {\n    r = gfnt_colr_scene(l, gshscene_list_clip_push_design_rect_fixed(l->list, l->frame, NULL, 0, box.x_min, box.y_min,\n        box.x_min, box.y_max, 0u, l->run->layer),",
     "a clip box of no width", None),
    (COLR, "box.x_min, box.y_min,\n        box.x_max, box.y_max, 0u, l->run->layer),", "box.y_min, box.x_min,\n        box.y_max, box.x_max, 0u, l->run->layer),",
     "a clip box's axes swapped", None),
    (COLR, "    r = gfnt_face_color_clip(l->face, l->glyph, &l->variation, &box, &found, l->error);",
     "    r = gfnt_face_color_clip(l->face, l->glyph, NULL, &box, &found, l->error);", "a clip box read at the default", None),
    (COLR, "  if (r == GFNT_OK && found) {\n    r = gfnt_colr_scene(l, gshscene_list_clip_pop(l->list)",
     "  if (0) {\n    r = gfnt_colr_scene(l, gshscene_list_clip_pop(l->list)", "a clip box left open", None),
    (COLR, "    l->bounded = true;\n    l->bound_ops = 0;\n  }\n  if (r == GFNT_OK) {\n    r = gfnt_colr_node(l, root, 0);",
     "  }\n  if (r == GFNT_OK) {\n    r = gfnt_colr_node(l, root, 0);", "a clip box not a bound", None),
    # L8: the glyph's group and its space.
    (COLR, "        encoded ? GSHSCENE_FIXED_TRANSFER_SRGB : GSHSCENE_FIXED_TRANSFER_LINEAR, l->run->layer),\n        \"the scene refused the glyph's group\");",
     "        (encoded ? 0 : 0) + GSHSCENE_FIXED_TRANSFER_SRGB, l->run->layer),\n        \"the scene refused the glyph's group\");", "the glyph composited in encoded values", None),
    (COLR, "        encoded ? GSHSCENE_FIXED_TRANSFER_SRGB : GSHSCENE_FIXED_TRANSFER_LINEAR, l->run->layer),\n        \"the scene refused the glyph's group\");",
     "        (encoded ? 0 : 0) + GSHSCENE_FIXED_TRANSFER_LINEAR, l->run->layer),\n        \"the scene refused the glyph's group\");", "the encoded space ignored for the group", None),
    (COLR, "  if (r == GFNT_OK) {\n    r = gfnt_colr_scene(l, gshscene_list_group_pop(l->list), \"the scene refused a group's pop\");\n  }\n  return r;\n}\n\n/** A box in the font",
     "  if (0) {\n    r = gfnt_colr_scene(l, gshscene_list_group_pop(l->list), \"the scene refused a group's pop\");\n  }\n  return r;\n}\n\n/** A box in the font",
     "the glyph's group left open", None),
    # L3: the gradient's space.
    (COLR, "    g.transfer = encoded ? GSHSCENE_FIXED_TRANSFER_SRGB : GSHSCENE_FIXED_TRANSFER_LINEAR;",
     "    g.transfer = GSHSCENE_FIXED_TRANSFER_SRGB;", "a gradient interpolated in encoded values", None),
    (COLR, "    g.premultiplied = encoded ? 0u : 1u;", "    g.premultiplied = 0u;", "a gradient interpolated straight", None),
    (COLR, "    g.premultiplied = encoded ? 0u : 1u;", "    g.premultiplied = 1u;", "the encoded space interpolated premultiplied", None),
    # L2: the gradients.
    (COLR, "    g.extend = (uint32_t)line.extend > 2u ? 0u : (uint32_t)line.extend;", "    g.extend = (uint32_t)line.extend > 2u ? 2u : (uint32_t)line.extend;",
     "an unknown extend taken for reflect", None),
    (COLR, "    g.extend = (uint32_t)line.extend > 2u ? 0u : (uint32_t)line.extend;", "    g.extend = (uint32_t)line.extend == 0u ? 2u : (uint32_t)line.extend == 2u ? 0u : (uint32_t)line.extend;",
     "pad and reflect swapped", None),
    (COLR, "      stops[i].offset = s.offset;", "      stops[i].offset = s.offset < 0 ? 0 : s.offset > 16384 ? 16384 : s.offset;", "a stop's offset clamped", None),
    (COLR, "      stops[i].color = gfnt_colr_paint(s.palette_index, s.alpha);", "      stops[i].color = gfnt_colr_paint(s.palette_index, GFNT_COLR_ONE);",
     "a stop's alpha dropped", None),
    (COLR, "      stops[i].bits = 14u;", "      stops[i].bits = 15u;", "a stop's offset at the wrong bits", None),
    (COLR, "      case GFNT_PAINT_LINEAR_GRADIENT:\n        g.kind = GSHSCENE_FIXED_GRADIENT_LINEAR;", "      case GFNT_PAINT_LINEAR_GRADIENT:\n        g.kind = GSHSCENE_FIXED_GRADIENT_RADIAL;",
     "a linear gradient as radial", None),
    (COLR, "        if (!gfnt_colr_plus(p->v[2], 16384, &g.v[2]) || !gfnt_colr_plus(p->v[3], 16384, &g.v[3])) {",
     "        g.v[2] = p->v[2];\n        g.v[3] = p->v[3];\n        if (0) {", "a sweep's angles without their bias", None),
    (COLR, "        if (!gfnt_colr_times(p->v[0], 16384, &g.v[0]) || !gfnt_colr_times(p->v[1], 16384, &g.v[1])) {",
     "        g.v[0] = p->v[0];\n        g.v[1] = p->v[1];\n        if (0) {", "a sweep's center at the wrong bits", None),
    (COLR, "    g.ops = t_count ? t : NULL;", "    (void)t;\n    g.ops = NULL;", "a gradient without its own transforms", None),
    # L1: the instance.
    (COLR, "  GFNT_Result r = gfnt_face_color_paint(l->face, ref, &l->variation, p, l->error);",
     "  GFNT_Result r = gfnt_face_color_paint(l->face, ref, NULL, p, l->error);", "a paint read at the default instance", None),
    (COLR, "    r = gfnt_face_color_stop(l->face, p, i, &l->variation, &s, l->error);",
     "    r = gfnt_face_color_stop(l->face, p, i, NULL, &s, l->error);", "a color stop read at the default instance", None),
    (COLR, "  if (r == GFNT_OK && p->variable) {\n    p->format = (GFNT_PaintFormat)((int)p->format - 1);\n  }",
     "", "a variable paint left in its Var format", None),
    (COLR, "  if (!gfnt_scene_instance_of(face, &l.variation)) {", "  if (0) {", "the instance not read from the face resource", None),
    # The transforms.
    (COLR, "      op->kind = GSHSCENE_FIXED_AFFINE;\n      op->bits = 16u;", "      op->kind = GSHSCENE_FIXED_AFFINE;\n      op->bits = 14u;",
     "Fixed read at 14 bits", None),
    (COLR, "      op->kind = GSHSCENE_FIXED_TRANSLATE;\n      op->v[0] = p->v[0];\n      op->v[1] = p->v[1];",
     "      op->kind = GSHSCENE_FIXED_TRANSLATE;\n      op->v[0] = p->v[1];\n      op->v[1] = p->v[0];",
     "a translation's axes", None),
    (COLR, "        ok = gfnt_colr_times(p->v[2], 16384, &op->v[2]) && gfnt_colr_times(p->v[3], 16384, &op->v[3]);",
     "        ok = gfnt_colr_times(p->v[2], 1, &op->v[2]) && gfnt_colr_times(p->v[3], 16384, &op->v[3]);", "a scale's center at the wrong bits", None),
    (COLR, "      op->v[1] = p->v[0];\n      if (p->format == GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER) {",
     "      op->v[1] = 16384;\n      if (p->format == GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER) {", "a uniform scale in x only", None),
    (COLR, "        ok = gfnt_colr_times(p->v[1], 16384, &op->v[2]) && gfnt_colr_times(p->v[2], 16384, &op->v[3]);",
     "        ok = gfnt_colr_times(p->v[2], 16384, &op->v[2]) && gfnt_colr_times(p->v[1], 16384, &op->v[3]);", "a uniform scale's center", None),
    (COLR, "      op->kind = GSHSCENE_FIXED_ROTATE;\n      op->bits = 15u;", "      op->kind = GSHSCENE_FIXED_ROTATE;\n      op->bits = 14u;",
     "an angle in turns rather than half turns", None),
    (COLR, "        ok = gfnt_colr_times(p->v[1], 32768, &op->v[1]) && gfnt_colr_times(p->v[2], 32768, &op->v[2]);",
     "        ok = gfnt_colr_times(p->v[1], 16384, &op->v[1]) && gfnt_colr_times(p->v[2], 32768, &op->v[2]);", "a rotation's center", None),
    (COLR, "      op->kind = GSHSCENE_FIXED_SKEW;\n      op->bits = 15u;\n      op->v[0] = p->v[0];\n      op->v[1] = p->v[1];",
     "      op->kind = GSHSCENE_FIXED_SKEW;\n      op->bits = 15u;\n      op->v[0] = p->v[1];\n      op->v[1] = p->v[0];",
     "a skew's angles", None),
    (COLR, "        ok = gfnt_colr_times(p->v[2], 32768, &op->v[2]) && gfnt_colr_times(p->v[3], 32768, &op->v[3]);",
     "        ok = gfnt_colr_times(p->v[3], 32768, &op->v[2]) && gfnt_colr_times(p->v[2], 32768, &op->v[3]);", "a skew's center", None),
    (COLR, "  int64_t w = (int64_t)v * k;\n", "  int64_t w = (int64_t)(int32_t)((uint32_t)v * (uint32_t)k);\n",
     "a product that wraps instead of failing", None),
    (COLR, "  l->ops[l->op_count++] = op;", "  l->ops[l->op_count] = op;", "a transform dropped", None),
    (COLR, "  r = gfnt_colr_node(l, p.child, depth + 1);\n  l->op_count--;", "  r = gfnt_colr_node(l, p.child, depth + 1);",
     "a transform kept past its subtree", None),
    # The walk.
    (COLR, "  if (depth > l->max_depth || ++l->nodes > GFNT_PAINT_WALK_NODES) {",
     "  if (depth > l->max_depth * 64u || ++l->nodes > GFNT_PAINT_WALK_NODES) {", "a depth bound 64 times too deep", None),
    (COLR, "      for (uint32_t i = 0; i < p.layer_count && r == GFNT_OK; ++i) {",
     "      for (uint32_t i = 1; i < p.layer_count && r == GFNT_OK; ++i) {", "a layer list's first layer skipped", None),
    (COLR, "      if (r == GFNT_OK && root == 0) {\n        return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_NO_RECORD,",
     "      if (0) {\n        return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_NO_RECORD,", "a color glyph with no record", None),
    (COLR, "        return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_UNBOUNDED, (uint32_t)p.format,",
     "        return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_NO_RECORD, (uint32_t)p.format,", "an unbounded paint's reason", None),
    (COLR, "  l->out->paint_format = format;", "  (void)format;\n  l->out->paint_format = 0;", "a refusal's format", None),
    # L5: a referenced glyph's own box.
    (COLR, "      if (r == GFNT_OK && found) {\n        r = gfnt_colr_scene(l, gshscene_list_clip_push_design_rect_fixed(l->list, l->frame,\n            l->op_count ? l->ops : NULL, l->op_count,",
     "      if (0) {\n        r = gfnt_colr_scene(l, gshscene_list_clip_push_design_rect_fixed(l->list, l->frame,\n            l->op_count ? l->ops : NULL, l->op_count,", "a referenced glyph's box ignored", None),
    (COLR, "            l->op_count ? l->ops : NULL, l->op_count, box.x_min, box.y_min, box.x_max, box.y_max, 0u,",
     "            NULL, 0, box.x_min, box.y_min, box.x_max, box.y_max, 0u,", "a referenced glyph's box without the transforms at the reference", None),
    (COLR, "        if (found) {\n          l->bounded = true;\n          l->bound_ops = l->op_count;\n        }\n        r = gfnt_colr_node(l, root, depth + 1);",
     "        r = gfnt_colr_node(l, root, depth + 1);", "a referenced glyph's box not a bound", None),
    (COLR, "      r = gfnt_face_color_base_paint(l->face, p.glyph, &root, l->error);\n      if (r == GFNT_OK && root == 0) {",
     "      r = gfnt_face_color_base_paint(l->face, p.glyph, &root, l->error);\n      if (0) {", "a color glyph with no record (the reference)", None),
    # The call.
    (COLR, "  out->source = root ? GFNT_COLOR_SOURCE_V1 : GFNT_COLOR_SOURCE_V0;",
     "  out->source = GFNT_COLOR_SOURCE_V1;", "the source", None),
    (COLR, "  if (r == GFNT_OK && root == 0) {\n    r = gfnt_face_color_layer_count(l.face, glyph, &layers, error);\n  }",
     "  if (r == GFNT_OK) {\n    r = gfnt_face_color_layer_count(l.face, glyph, &layers, error);\n    root = layers ? 0 : root;\n  }",
     "version 0 preferred to version 1", None),
    (COLR, "  r = root ? gfnt_colr_graph(&l, root) : gfnt_colr_layers(&l, layers);",
     "  r = root ? gfnt_colr_layers(&l, layers) : gfnt_colr_graph(&l, root);",
     "each version lowered by the other's walk", None),
    (COLR, "    if (gshscene_list_truncate(list, before) != GSHSCENE_OK) {", "    if (0) {", "a refusal leaves what it recorded", None),
    (COLR, "    return r;\n  }\n  out->items = gshscene_list_count(list) - before;\n  return GFNT_OK;", "    return r;\n  }\n  out->items = gshscene_list_count(list);\n  return GFNT_OK;",
     "the items counted from zero", None),
    # color-paint story P5 (R3, R4, R5; L12, L13): what a refused glyph is given.
    (COLR, "    if (r == GFNT_ERR_LIMIT && l.walk_exceeded) {", "    if (0) {", "the walk's bound answered as the call's limit", None),
    (COLR, "      out->refusal = (uint32_t)GFNT_COLOR_REFUSED_WALK_LIMIT;", "      out->refusal = (uint32_t)GFNT_COLOR_REFUSED_UNBOUNDED;",
     "the walk's bound given the wrong reason", None),
    (COLR, "        && run->refusal_policy != (uint32_t)GFNT_COLOR_ON_REFUSAL_NONE) {", "        ) {", "the none policy recording a placeholder", None),
    (COLR, "      r = gfnt_colr_refusal_record(&l, run->refusal_policy, before);", "      r = gfnt_colr_refusal_record(&l, GFNT_COLOR_ON_REFUSAL_PLACEHOLDER, before);",
     "the fallback policy ignored", None),
    (COLR, "  if (r == GFNT_OK && policy == GFNT_COLOR_ON_REFUSAL_FALLBACK) {", "  (void)policy;\n  if (r == GFNT_OK) {", "the default policy drawing the stand-in", None),
    (COLR, "    const bool approximated = has_outline;", "    const bool approximated = true;", "a placeholder approximated when nothing is drawn", None),
    (COLR, "    const bool approximated = has_outline;", "    const bool approximated = false;", "a fallback reported as refused", None),
    (COLR, "    *has = gfnt_outline_point_count(outline) > 0;", "    *has = true;", "a fallback drawn for a glyph with no outline", None),
    (COLR, "  l->out->outcome = has_outline ? GFNT_COLOR_OUTCOME_FALLBACK : GFNT_COLOR_OUTCOME_PLACEHOLDER;",
     "  l->out->outcome = has_outline ? GFNT_COLOR_OUTCOME_PLACEHOLDER : GFNT_COLOR_OUTCOME_FALLBACK;", "the outcome exchanged", None),
    (COLR, "GSHSCENE_PLACEHOLDER_SOURCE_COLR, (uint16_t)l->out->refusal,", "GSHSCENE_PLACEHOLDER_SOURCE_COLR, 1u,", "the placeholder's code not the refusal's", None),
    (COLR, "GSHSCENE_PLACEHOLDER_SOURCE_COLR, (uint16_t)l->out->refusal,", "GSHSCENE_PLACEHOLDER_SOURCE_COLR + 1u, (uint16_t)l->out->refusal,",
     "the placeholder's source", None),
    (COLR, "approximated ? 1u : 0u, l->run->layer),", "approximated ? 1u : 0u, 0),", "the placeholder's layer", None),
    (COLR, "      r = gfnt_colr_scene(l, gshscene_list_fill_fixed(l->list, outline_run, NULL, 0, GSHGEOM_FILL_NONZERO, &foreground,\n          l->run->layer),",
     "      r = gfnt_colr_scene(l, gshscene_list_fill_fixed(l->list, outline_run, NULL, 0, GSHGEOM_FILL_NONZERO, &foreground,\n          0),",
     "the fallback fill's layer", None),
    (COLR, "    foreground.kind = GSHSCENE_FIXED_PAINT_FOREGROUND;", "    foreground.kind = GSHSCENE_FIXED_PAINT_PALETTE;", "the fallback in a palette entry, not the foreground", None),
    (COLR, "    foreground.alpha = GFNT_COLR_ONE;", "    foreground.alpha = GFNT_COLR_ONE / 2;", "the fallback at half alpha", None),
    (COLR, "  if (r == GFNT_OK && found && clip.x_min <= clip.x_max && clip.y_min <= clip.y_max) {", "  if (0) {", "the clip box not the placeholder's first box", None),
    (COLR, "  if (gfnt_glyph_extents(l->face, l->glyph, &l->variation, &e) && (e.width != 0 || e.height != 0)) {", "  if (0) {",
     "the extents not the placeholder's second box", None),
    (COLR, "      box->bits = 6u;", "      box->bits = 0u;", "the outline's 26.6 box read as whole units", None),
    (COLR, "    const int64_t y0 = y1 + e.height;   // the height is negative: y up", "    const int64_t y0 = y1 - e.height;   // the height is negative: y up",
     "the extents' height read the wrong way", None),
    (COLR, "  if (!face || !store || !list || !run || !out || run->refusal_policy >= (uint32_t)GFNT_COLOR_ON_REFUSAL_COUNT) {",
     "  if (!face || !store || !list || !run || !out) {", "a policy past the enum accepted", None),
    (COLR, "      gfnt_error_clear(error);\n      out->items = gshscene_list_count(list) - before;", "      out->items = gshscene_list_count(list) - before;",
     "the refusal's diagnostic left after the call succeeded", None),
    (COLR, "  gshscene_resource_release(l.plane);\n  gshscene_resource_release(l.frame);\n", "  gshscene_resource_release(l.frame);\n",
     "the plane's reference leaked", None),
    # (A mutant that dropped the first check of an unbound face was removed: the check that follows it, of the face's
    # instance, fails on the same face with the same result and message, so the program's behavior is the same.)
    # The palette.
    (COLR, "      rgba[4u * i] = c.red;\n      rgba[4u * i + 1u] = c.green;",
     "      rgba[4u * i] = c.blue;\n      rgba[4u * i + 1u] = c.green;", "red read as blue", None),
    (COLR, "      rgba[4u * i + 3u] = c.alpha;", "      rgba[4u * i + 3u] = 255;", "an entry's alpha", None),
    (COLR, "    r = gfnt_face_palette_color(f, palette, i, &c, error);", "    r = gfnt_face_palette_color(f, 0, i, &c, error);",
     "every palette read as the first", None),
    # The binding.
    (SCENE, "      || ops->glyph_outline != gfnt_scene_ops.glyph_outline || !ctx) {", "      || !ctx) {",
     "another provider's binding taken for this library's", None),
    (SCENE, "  *out = gfnt_scene_variation((const GFNT_SceneBinding *)ctx);\n  return true;", "  memset(out, 0, sizeof *out);\n  return true;",
     "the binding's instance not read", None),
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
            if colr_scene_diff.differ(got[gid], ref[gid], colr_scene_diff.Ties(False)):
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
