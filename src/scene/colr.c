/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Font.
 *
 * Ghoti.io Font is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Font is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Color glyphs lowered into a `shape-scene` list (story 14c; scene_color.h,
 * documentation/design.md section 8.8): a `CPAL` palette as a palette resource,
 * and a glyph's `COLR` recorded through `shape-scene`'s float-free calls only
 * (fixed.h), so that this file names no floating type.
 */

#include <ghoti.io/font/macros.h>
#include <ghoti.io/font/scene_color.h>

#include <ghoti.io/font/color.h>
#include <ghoti.io/font/face.h>
#include <ghoti.io/shape-scene/fixed.h>
#include <ghoti.io/shape-scene/glyph_provider.h>
#include <stdbool.h>
#include <string.h>
#include "../sfnt/sfnt.h"
#include "binding.h"

#define GFNT_COLR_TAG GFNT_TAG('C', 'O', 'L', 'R')

/** `COLR`'s composite modes 0 to 12 (clear to plus) as the scene's operators (N4: source-over is 0 there). */
static const uint32_t gfnt_colr_operator[13] = {
  1u,   // clear
  2u,   // src
  3u,   // dest
  0u,   // src over
  4u,   // dest over
  5u,   // src in
  6u,   // dest in
  7u,   // src out
  8u,   // dest out
  9u,   // src atop
  10u,  // dest atop
  11u,  // xor
  12u,  // plus
};

/** F2.14's one: a solid's alpha is clamped to `[0, GFNT_COLR_ONE]`. */
#define GFNT_COLR_ONE 16384

/** The state of one lowering. */
typedef struct GFNT_ColrLower {
  const GFNT_Face * face;
  const GSHSCENE_Resource * face_resource;
  GSHSCENE_Store * store;
  GSHSCENE_List * list;
  const GFNT_ColorRun * run;
  GFNT_ColorLowering * out;
  GFNT_Error * error;
  uint32_t glyph;               ///< The base glyph, for the diagnostics.
  GSHSCENE_Fixed_Op ops[GSHSCENE_FIXED_MAX_OPS];  ///< The transforms above the node, outermost first.
  size_t op_count;
  size_t nodes;                 ///< The nodes visited, bounded by ::GFNT_PAINT_WALK_NODES.
  size_t max_depth;
} GFNT_ColrLower;

/** The seam's failure in this library's spelling. */
static GFNT_Result gfnt_colr_from(GSHSCENE_Result r) {
  switch (r) {
    case GSHSCENE_OK:
      return GFNT_OK;
    case GSHSCENE_ERR_LIMIT:
      return GFNT_ERR_LIMIT;
    case GSHSCENE_ERR_OOM:
      return GFNT_ERR_OOM;
    default:
      return GFNT_ERR_INVALID;
  }
}

static GFNT_Result gfnt_colr_scene(GFNT_ColrLower * l, GSHSCENE_Result r, const char * what) {
  if (r == GSHSCENE_OK) {
    return GFNT_OK;
  }
  return gfnt_error_set(l->error, gfnt_colr_from(r), 0, 0, l->glyph, what);
}

static GFNT_Result gfnt_colr_refuse(GFNT_ColrLower * l, GFNT_ColorRefusal why,
    uint32_t format, const char * what) {
  l->out->refusal = (uint32_t)why;
  l->out->paint_format = format;
  return gfnt_error_set(l->error, GFNT_ERR_UNSUPPORTED, GFNT_COLR_TAG, 0, l->glyph, what);
}

/** A one-glyph run of the face at the run's size and origin. */
static GFNT_Result gfnt_colr_run_of(GFNT_ColrLower * l, uint32_t glyph,
    const GSHSCENE_Resource ** out) {
  GSHSCENE_Glyph_Fixed g;

  g.id = glyph;
  g.x = l->run->x;
  g.y = l->run->y;
  return gfnt_colr_scene(l, gshscene_store_intern_glyph_run_fixed(l->store, l->face_resource,
      l->run->size, l->run->hint_mode, l->run->hint_version, l->run->bits, 1, &g, out),
      "the scene refused a layer's glyph run");
}

/** A palette entry, or the foreground (`0xFFFF`, which fixed.h lowers), at an F2.14 alpha clamped to [0, 1]. */
static GSHSCENE_Paint_Fixed gfnt_colr_paint(uint16_t entry, int32_t alpha) {
  GSHSCENE_Paint_Fixed p;

  p.kind = GSHSCENE_FIXED_PAINT_PALETTE;
  p.entry = entry;
  p.alpha = alpha < 0 ? 0 : alpha > GFNT_COLR_ONE ? GFNT_COLR_ONE : alpha;
  p.bits = 14u;
  return p;
}

/** Fill @p glyph under the transforms in force with @p paint. */
static GFNT_Result gfnt_colr_fill(GFNT_ColrLower * l, uint32_t glyph,
    const GSHSCENE_Paint_Fixed * paint) {
  const GSHSCENE_Resource * shape = NULL;
  GFNT_Result r = gfnt_colr_run_of(l, glyph, &shape);

  if (r != GFNT_OK) {
    return r;
  }
  r = gfnt_colr_scene(l, gshscene_list_fill_fixed(l->list, shape, l->op_count ? l->ops : NULL,
      l->op_count, GSHGEOM_FILL_NONZERO, paint, l->run->layer), "the scene refused a fill");
  gshscene_resource_release(shape);   // the list holds its own
  return r;
}

/**
 * The design-map op of a transform paint (scene_color.h): F2.14 scales at 14
 * bits, angles in half turns at 15 (a turn is 2^15), `Fixed` at 16, centers in
 * font units carried to the op's bits by a multiplication.
 */
static bool gfnt_colr_op(const GFNT_ColorPaint * p, GSHSCENE_Fixed_Op * op) {
  memset(op, 0, sizeof *op);
  switch (p->format) {
    case GFNT_PAINT_TRANSFORM:
      op->kind = GSHSCENE_FIXED_AFFINE;
      op->bits = 16u;
      memcpy(op->v, p->v, sizeof op->v);
      return true;
    case GFNT_PAINT_TRANSLATE:
      op->kind = GSHSCENE_FIXED_TRANSLATE;
      op->v[0] = p->v[0];
      op->v[1] = p->v[1];
      return true;
    case GFNT_PAINT_SCALE:
    case GFNT_PAINT_SCALE_AROUND_CENTER:
      op->kind = GSHSCENE_FIXED_SCALE;
      op->bits = 14u;
      op->v[0] = p->v[0];
      op->v[1] = p->v[1];
      if (p->format == GFNT_PAINT_SCALE_AROUND_CENTER) {
        op->v[2] = p->v[2] * 16384;
        op->v[3] = p->v[3] * 16384;
      }
      return true;
    case GFNT_PAINT_SCALE_UNIFORM:
    case GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER:
      op->kind = GSHSCENE_FIXED_SCALE;
      op->bits = 14u;
      op->v[0] = p->v[0];
      op->v[1] = p->v[0];
      if (p->format == GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER) {
        op->v[2] = p->v[1] * 16384;
        op->v[3] = p->v[2] * 16384;
      }
      return true;
    case GFNT_PAINT_ROTATE:
    case GFNT_PAINT_ROTATE_AROUND_CENTER:
      op->kind = GSHSCENE_FIXED_ROTATE;
      op->bits = 15u;
      op->v[0] = p->v[0];
      if (p->format == GFNT_PAINT_ROTATE_AROUND_CENTER) {
        op->v[1] = p->v[1] * 32768;
        op->v[2] = p->v[2] * 32768;
      }
      return true;
    case GFNT_PAINT_SKEW:
    case GFNT_PAINT_SKEW_AROUND_CENTER:
      op->kind = GSHSCENE_FIXED_SKEW;
      op->bits = 15u;
      op->v[0] = p->v[0];
      op->v[1] = p->v[1];
      if (p->format == GFNT_PAINT_SKEW_AROUND_CENTER) {
        op->v[2] = p->v[2] * 32768;
        op->v[3] = p->v[3] * 32768;
      }
      return true;
    default:
      return false;
  }
}

static GFNT_Result gfnt_colr_node(GFNT_ColrLower * l, GFNT_PaintRef ref, size_t depth);

/** Read a node, counting it against the walk's bounds. */
static GFNT_Result gfnt_colr_read(GFNT_ColrLower * l, GFNT_PaintRef ref, size_t depth,
    GFNT_ColorPaint * p) {
  if (depth > l->max_depth || ++l->nodes > GFNT_PAINT_WALK_NODES) {
    return gfnt_error_set(l->error, GFNT_ERR_LIMIT, GFNT_COLR_TAG, ref, l->glyph,
        "a paint graph past the walk's depth or node bound");
  }
  GFNT_Result r = gfnt_face_color_paint(l->face, ref, NULL, p, l->error);
  if (r == GFNT_ERR_UNSUPPORTED) {
    return gfnt_error_set(l->error, GFNT_ERR_CORRUPT, GFNT_COLR_TAG, ref, l->glyph,
        "a paint format COLR does not define");
  }
  if (r == GFNT_OK && p->variable) {
    return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_VARIABLE, (uint32_t)p->format,
        "a variable paint: the scene lowers the default forms only");
  }
  return r;
}

/** `PaintGlyph`: a fill when its child is a solid under transforms, else a clip around the child. */
static GFNT_Result gfnt_colr_glyph(GFNT_ColrLower * l, const GFNT_ColorPaint * p, size_t depth) {
  GFNT_ColorPaint c;
  GFNT_PaintRef ref = p->child;
  size_t d = depth + 1;
  GFNT_Result r;

  // A transform between a glyph and its solid moves the solid, which fills the
  // glyph all the same: look through them.
  for (;;) {
    r = gfnt_colr_read(l, ref, d, &c);
    if (r != GFNT_OK) {
      return r;
    }
    GSHSCENE_Fixed_Op ignored;
    if (!gfnt_colr_op(&c, &ignored)) {
      break;
    }
    ref = c.child;
    ++d;
  }
  if (c.format == GFNT_PAINT_SOLID) {
    GSHSCENE_Paint_Fixed paint = gfnt_colr_paint(c.palette_index, c.v[0]);
    return gfnt_colr_fill(l, p->glyph, &paint);
  }
  const GSHSCENE_Resource * shape = NULL;
  r = gfnt_colr_run_of(l, p->glyph, &shape);
  if (r != GFNT_OK) {
    return r;
  }
  r = gfnt_colr_scene(l, gshscene_list_clip_push_fixed(l->list, shape, l->op_count ? l->ops : NULL,
      l->op_count, GSHGEOM_FILL_NONZERO, l->run->layer), "the scene refused a glyph's clip");
  gshscene_resource_release(shape);
  if (r == GFNT_OK) {
    r = gfnt_colr_node(l, p->child, depth + 1);
  }
  if (r == GFNT_OK) {
    r = gfnt_colr_scene(l, gshscene_list_clip_pop(l->list), "the scene refused a clip's pop");
  }
  return r;
}

/** `PaintComposite`: the backdrop and, composited onto it by the operator, the source, isolated together. */
static GFNT_Result gfnt_colr_composite(GFNT_ColrLower * l, const GFNT_ColorPaint * p, size_t depth) {
  if (p->composite_mode > 12u) {
    l->out->composite_mode = p->composite_mode;
    return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_BLEND_MODE, (uint32_t)p->format,
        "a blend mode: the scene composites the Porter-Duff operators and plus only");
  }
  GFNT_Result r = gfnt_colr_scene(l, gshscene_list_group_push_fixed(l->list, 0u, l->run->layer),
      "the scene refused a composite's group");
  if (r == GFNT_OK) {
    r = gfnt_colr_node(l, p->backdrop, depth + 1);
  }
  if (r == GFNT_OK) {
    r = gfnt_colr_scene(l, gshscene_list_group_push_fixed(l->list,
        gfnt_colr_operator[p->composite_mode], l->run->layer), "the scene refused a source's group");
  }
  if (r == GFNT_OK) {
    r = gfnt_colr_node(l, p->child, depth + 1);
  }
  if (r == GFNT_OK) {
    r = gfnt_colr_scene(l, gshscene_list_group_pop(l->list), "the scene refused a group's pop");
  }
  if (r == GFNT_OK) {
    r = gfnt_colr_scene(l, gshscene_list_group_pop(l->list), "the scene refused a group's pop");
  }
  return r;
}

static GFNT_Result gfnt_colr_node(GFNT_ColrLower * l, GFNT_PaintRef ref, size_t depth) {
  GFNT_ColorPaint p;
  GFNT_Result r = gfnt_colr_read(l, ref, depth, &p);

  if (r != GFNT_OK) {
    return r;
  }
  switch (p.format) {
    case GFNT_PAINT_COLR_LAYERS:
      for (uint32_t i = 0; i < p.layer_count && r == GFNT_OK; ++i) {
        GFNT_PaintRef layer = 0;
        r = gfnt_face_color_layer_paint(l->face, (size_t)p.first_layer + i, &layer, l->error);
        if (r == GFNT_OK) {
          r = gfnt_colr_node(l, layer, depth + 1);
        }
      }
      return r;
    case GFNT_PAINT_SOLID:
      return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_UNBOUNDED, (uint32_t)p.format,
          "a solid no glyph bounds: it would fill the plane");
    case GFNT_PAINT_LINEAR_GRADIENT:
    case GFNT_PAINT_RADIAL_GRADIENT:
    case GFNT_PAINT_SWEEP_GRADIENT:
      return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_GRADIENT, (uint32_t)p.format,
          "a gradient: the scene has none yet");
    case GFNT_PAINT_GLYPH:
      return gfnt_colr_glyph(l, &p, depth);
    case GFNT_PAINT_COLR_GLYPH: {
      GFNT_PaintRef root = 0;
      r = gfnt_face_color_base_paint(l->face, p.glyph, &root, l->error);
      if (r == GFNT_OK && root == 0) {
        return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_NO_RECORD, (uint32_t)p.format,
            "a color glyph naming a glyph with no version 1 record");
      }
      return r == GFNT_OK ? gfnt_colr_node(l, root, depth + 1) : r;
    }
    case GFNT_PAINT_COMPOSITE:
      return gfnt_colr_composite(l, &p, depth);
    default:
      break;
  }
  GSHSCENE_Fixed_Op op;
  if (!gfnt_colr_op(&p, &op)) {
    return gfnt_error_set(l->error, GFNT_ERR_CORRUPT, GFNT_COLR_TAG, ref, l->glyph,
        "a paint format COLR does not define");
  }
  if (l->op_count == GSHSCENE_FIXED_MAX_OPS) {
    return gfnt_error_set(l->error, GFNT_ERR_LIMIT, GFNT_COLR_TAG, ref, l->glyph,
        "more transforms above a shape than the scene's design map holds");
  }
  l->ops[l->op_count++] = op;
  r = gfnt_colr_node(l, p.child, depth + 1);
  l->op_count--;
  return r;
}

/** Version 0: each layer, bottom first, a fill with its entry. */
static GFNT_Result gfnt_colr_layers(GFNT_ColrLower * l, size_t count) {
  GFNT_Result r = GFNT_OK;

  for (size_t i = 0; i < count && r == GFNT_OK; ++i) {
    GFNT_ColorLayer layer;
    r = gfnt_face_color_layer(l->face, l->glyph, i, &layer, l->error);
    if (r == GFNT_OK) {
      GSHSCENE_Paint_Fixed paint = gfnt_colr_paint(layer.palette_index, GFNT_COLR_ONE);
      r = gfnt_colr_fill(l, layer.glyph, &paint);
    }
  }
  return r;
}

GFNT_Result gfnt_scene_record_color_glyph(const GSHSCENE_Resource * face,
    GSHSCENE_Store * store, GSHSCENE_List * list, uint32_t glyph,
    const GFNT_ColorRun * run, GFNT_ColorLowering * out, GFNT_Error * error) {
  GFNT_ColrLower l;
  GFNT_PaintRef root = 0;
  size_t layers = 0;
  GFNT_Result r;

  gfnt_error_clear(error);
  if (!face || !store || !list || !run || !out) {
    return GFNT_ERR_INVALID;
  }
  memset(out, 0, sizeof *out);
  memset(&l, 0, sizeof l);
  l.face = gfnt_scene_face_of(face);
  if (!l.face) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph,
        "a face resource this library's provider does not bind");
  }
  l.face_resource = face;
  l.store = store;
  l.list = list;
  l.run = run;
  l.out = out;
  l.error = error;
  l.glyph = glyph;
  l.max_depth = l.face->limits.max_paint_depth;
  if (!gfnt_face_has_color_layers(l.face)) {
    return GFNT_OK;   // no COLR: not a color glyph
  }
  r = gfnt_face_color_base_paint(l.face, glyph, &root, error);
  if (r == GFNT_OK && root == 0) {
    r = gfnt_face_color_layer_count(l.face, glyph, &layers, error);
  }
  if (r != GFNT_OK) {
    return r;
  }
  if (root == 0 && layers == 0) {
    return GFNT_OK;   // a glyph of a color font that is not a color glyph
  }
  out->source = root ? GFNT_COLOR_SOURCE_V1 : GFNT_COLOR_SOURCE_V0;
  size_t before = gshscene_list_count(list);
  r = root ? gfnt_colr_node(&l, root, 0) : gfnt_colr_layers(&l, layers);
  if (r != GFNT_OK) {
    // All or nothing: what was recorded goes (a refusal says why in `out`).
    if (gshscene_list_truncate(list, before) != GSHSCENE_OK) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph, "the list could not be truncated back");
    }
    return r;
  }
  out->items = gshscene_list_count(list) - before;
  return GFNT_OK;
}

GFNT_Result gfnt_scene_intern_palette(const GSHSCENE_Resource * face,
    GSHSCENE_Store * store, size_t palette, const GSHSCENE_Resource ** out_palette,
    GFNT_Error * error) {
  const GFNT_Face * f;
  size_t palettes = 0;
  size_t entries = 0;
  GFNT_Result r;

  gfnt_error_clear(error);
  if (!face || !store || !out_palette) {
    return GFNT_ERR_INVALID;
  }
  f = gfnt_scene_face_of(face);
  if (!f) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a face resource this library's provider does not bind");
  }
  r = gfnt_face_palette_count(f, &palettes, error);
  if (r == GFNT_OK) {
    r = gfnt_face_palette_entry_count(f, &entries, error);
  }
  if (r != GFNT_OK) {
    return r;
  }
  if (palette >= palettes) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, GFNT_TAG('C', 'P', 'A', 'L'), 0,
        GFNT_GLYPH_NONE, "a palette past the face's count");
  }
  uint8_t * rgba = NULL;
  if (entries) {
    rgba = f->allocator->malloc_fn(f->allocator->ctx, 4u * entries);
    if (!rgba) {
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory for a palette's entries");
    }
  }
  for (size_t i = 0; i < entries && r == GFNT_OK; ++i) {
    GFNT_Color c;
    r = gfnt_face_palette_color(f, palette, i, &c, error);
    if (r == GFNT_OK) {
      rgba[4u * i] = c.red;
      rgba[4u * i + 1u] = c.green;
      rgba[4u * i + 2u] = c.blue;
      rgba[4u * i + 3u] = c.alpha;
    }
  }
  if (r == GFNT_OK) {
    GSHSCENE_Result s = gshscene_store_intern_palette_rgba8(store, entries, rgba, out_palette);
    if (s != GSHSCENE_OK) {
      r = gfnt_error_set(error, s == GSHSCENE_ERR_LIMIT ? GFNT_ERR_LIMIT
          : s == GSHSCENE_ERR_OOM ? GFNT_ERR_OOM : GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "the scene's store refused the palette");
    }
  }
  if (rgba) {
    f->allocator->free_fn(f->allocator->ctx, rgba);
  }
  return r;
}
