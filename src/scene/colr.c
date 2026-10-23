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
 * Color glyphs lowered into a `shape-scene` list (story 14c and color-paint
 * story P4; scene_color.h, documentation/design.md section 8.8): a `CPAL`
 * palette as a palette resource, and a glyph's `COLR` recorded through
 * `shape-scene`'s float-free calls only (fixed.h), so that this file names no
 * floating type. The rules are L1 to L11 of scene_color.h and design.md 8.8.
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

/** The scene's operator for a `PaintComposite` mode the standard does not define: `COMPOSITE_CLEAR` (L7). */
#define GFNT_COLR_UNKNOWN_MODE_OPERATOR 1u

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
  GFNT_Variation variation;     ///< The face resource's instance, which every `Var*` number is read at (L1).
  const GSHSCENE_Resource * frame;  ///< A one-glyph run of the base glyph: it places the design space (fixed.h).
  const GSHSCENE_Resource * plane;  ///< The store's plane, once a paint needs it (L6).
  GSHSCENE_Fixed_Op ops[GSHSCENE_FIXED_MAX_OPS];  ///< The transforms above the node, outermost first.
  size_t op_count;
  bool bounded;                 ///< Whether a clip this lowering pushed (a glyph's, a box) is in force (L6).
  size_t bound_ops;             ///< The transforms above the innermost such clip.
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

/** `v * k` if it fits an `int32_t`: a `Var*` number can be any 32-bit value, and a product must not wrap. */
static bool gfnt_colr_times(int32_t v, int32_t k, int32_t * out) {
  int64_t w = (int64_t)v * k;

  if (w > INT32_MAX || w < INT32_MIN) {
    return false;
  }
  *out = (int32_t)w;
  return true;
}

/** `v + k` if it fits an `int32_t`. */
static bool gfnt_colr_plus(int32_t v, int32_t k, int32_t * out) {
  int64_t w = (int64_t)v + k;

  if (w > INT32_MAX || w < INT32_MIN) {
    return false;
  }
  *out = (int32_t)w;
  return true;
}

/** How a transform paint's numbers reach a design-map op. */
typedef enum GFNT_ColrOpResult {
  GFNT_COLR_OP_NOT_A_TRANSFORM = 0,
  GFNT_COLR_OP_OK = 1,
  GFNT_COLR_OP_RANGE = 2    ///< A number past the op's fixed range (a variation's delta can be any size).
} GFNT_ColrOpResult;

/**
 * The design-map op of a transform paint (scene_color.h): F2.14 scales at 14
 * bits, angles in half turns at 15 (a turn is 2^15), `Fixed` at 16, centers in
 * font units carried to the op's bits by a multiplication. The paint's format is
 * the base one (the `Var` forms are normalised when a node is read, L1).
 */
static GFNT_ColrOpResult gfnt_colr_op(const GFNT_ColorPaint * p, GSHSCENE_Fixed_Op * op) {
  bool ok = true;

  memset(op, 0, sizeof *op);
  switch (p->format) {
    case GFNT_PAINT_TRANSFORM:
      op->kind = GSHSCENE_FIXED_AFFINE;
      op->bits = 16u;
      memcpy(op->v, p->v, sizeof op->v);
      return GFNT_COLR_OP_OK;
    case GFNT_PAINT_TRANSLATE:
      op->kind = GSHSCENE_FIXED_TRANSLATE;
      op->v[0] = p->v[0];
      op->v[1] = p->v[1];
      return GFNT_COLR_OP_OK;
    case GFNT_PAINT_SCALE:
    case GFNT_PAINT_SCALE_AROUND_CENTER:
      op->kind = GSHSCENE_FIXED_SCALE;
      op->bits = 14u;
      op->v[0] = p->v[0];
      op->v[1] = p->v[1];
      if (p->format == GFNT_PAINT_SCALE_AROUND_CENTER) {
        ok = gfnt_colr_times(p->v[2], 16384, &op->v[2]) && gfnt_colr_times(p->v[3], 16384, &op->v[3]);
      }
      return ok ? GFNT_COLR_OP_OK : GFNT_COLR_OP_RANGE;
    case GFNT_PAINT_SCALE_UNIFORM:
    case GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER:
      op->kind = GSHSCENE_FIXED_SCALE;
      op->bits = 14u;
      op->v[0] = p->v[0];
      op->v[1] = p->v[0];
      if (p->format == GFNT_PAINT_SCALE_UNIFORM_AROUND_CENTER) {
        ok = gfnt_colr_times(p->v[1], 16384, &op->v[2]) && gfnt_colr_times(p->v[2], 16384, &op->v[3]);
      }
      return ok ? GFNT_COLR_OP_OK : GFNT_COLR_OP_RANGE;
    case GFNT_PAINT_ROTATE:
    case GFNT_PAINT_ROTATE_AROUND_CENTER:
      op->kind = GSHSCENE_FIXED_ROTATE;
      op->bits = 15u;
      op->v[0] = p->v[0];
      if (p->format == GFNT_PAINT_ROTATE_AROUND_CENTER) {
        ok = gfnt_colr_times(p->v[1], 32768, &op->v[1]) && gfnt_colr_times(p->v[2], 32768, &op->v[2]);
      }
      return ok ? GFNT_COLR_OP_OK : GFNT_COLR_OP_RANGE;
    case GFNT_PAINT_SKEW:
    case GFNT_PAINT_SKEW_AROUND_CENTER:
      op->kind = GSHSCENE_FIXED_SKEW;
      op->bits = 15u;
      op->v[0] = p->v[0];
      op->v[1] = p->v[1];
      if (p->format == GFNT_PAINT_SKEW_AROUND_CENTER) {
        ok = gfnt_colr_times(p->v[2], 32768, &op->v[2]) && gfnt_colr_times(p->v[3], 32768, &op->v[3]);
      }
      return ok ? GFNT_COLR_OP_OK : GFNT_COLR_OP_RANGE;
    default:
      return GFNT_COLR_OP_NOT_A_TRANSFORM;
  }
}

static bool gfnt_colr_is_leaf(GFNT_PaintFormat f) {
  return f == GFNT_PAINT_SOLID || f == GFNT_PAINT_LINEAR_GRADIENT || f == GFNT_PAINT_RADIAL_GRADIENT
      || f == GFNT_PAINT_SWEEP_GRADIENT;
}

/** Whether the op of a transform paint can be made, failing the lowering where its numbers are past the range. */
static GFNT_Result gfnt_colr_make_op(GFNT_ColrLower * l, const GFNT_ColorPaint * p, GFNT_PaintRef ref,
    GSHSCENE_Fixed_Op * op, bool * is_transform) {
  GFNT_ColrOpResult o = gfnt_colr_op(p, op);

  *is_transform = o != GFNT_COLR_OP_NOT_A_TRANSFORM;
  if (o == GFNT_COLR_OP_RANGE) {
    return gfnt_error_set(l->error, GFNT_ERR_LIMIT, GFNT_COLR_TAG, ref, l->glyph,
        "a transform's number past the scene's fixed range");
  }
  return GFNT_OK;
}

static GFNT_Result gfnt_colr_node(GFNT_ColrLower * l, GFNT_PaintRef ref, size_t depth);

/**
 * Read a node, counting it against the walk's bounds, at the face resource's instance (L1); a `Var` form comes back
 * as its base format with `variable` set, its numbers already carrying the deltas.
 */
static GFNT_Result gfnt_colr_read(GFNT_ColrLower * l, GFNT_PaintRef ref, size_t depth,
    GFNT_ColorPaint * p) {
  if (depth > l->max_depth || ++l->nodes > GFNT_PAINT_WALK_NODES) {
    return gfnt_error_set(l->error, GFNT_ERR_LIMIT, GFNT_COLR_TAG, ref, l->glyph,
        "a paint graph past the walk's depth or node bound");
  }
  GFNT_Result r = gfnt_face_color_paint(l->face, ref, &l->variation, p, l->error);
  if (r == GFNT_ERR_UNSUPPORTED) {
    return gfnt_error_set(l->error, GFNT_ERR_CORRUPT, GFNT_COLR_TAG, ref, l->glyph,
        "a paint format COLR does not define");
  }
  if (r == GFNT_OK && p->variable) {
    p->format = (GFNT_PaintFormat)((int)p->format - 1);
  }
  return r;
}

/** The store's plane, interned the first time a paint no glyph bounds needs it. */
static GFNT_Result gfnt_colr_plane(GFNT_ColrLower * l) {
  if (l->plane) {
    return GFNT_OK;
  }
  return gfnt_colr_scene(l, gshscene_store_intern_plane(l->store, &l->plane), "the scene refused the plane");
}

/** The gradient resource of a gradient paint, placed under the transforms @p t (L2). */
static GFNT_Result gfnt_colr_gradient(GFNT_ColrLower * l, const GFNT_ColorPaint * p, const GSHSCENE_Fixed_Op * t,
    size_t t_count, const GSHSCENE_Resource ** out) {
  GFNT_ColorLine line;
  GSHSCENE_Gradient_Fixed g;
  GSHSCENE_Gradient_Stop_Fixed * stops = NULL;
  GFNT_Result r = gfnt_face_color_line(l->face, p, &line, l->error);
  const bool encoded = l->run->space == GFNT_COLOR_SPACE_ENCODED;

  if (r != GFNT_OK) {
    return r;
  }
  memset(&g, 0, sizeof g);
  if (line.stop_count) {
    stops = l->face->allocator->malloc_fn(l->face->allocator->ctx, line.stop_count * sizeof *stops);
    if (!stops) {
      return gfnt_error_set(l->error, GFNT_ERR_OOM, GFNT_COLR_TAG, 0, l->glyph, "no memory for a color line");
    }
  }
  for (size_t i = 0; i < line.stop_count && r == GFNT_OK; ++i) {
    GFNT_ColorStop s;

    r = gfnt_face_color_stop(l->face, p, i, &l->variation, &s, l->error);
    if (r == GFNT_OK) {
      stops[i].offset = s.offset;
      stops[i].bits = 14u;
      stops[i].color = gfnt_colr_paint(s.palette_index, s.alpha);
    }
  }
  if (r == GFNT_OK) {
    switch (p->format) {
      case GFNT_PAINT_LINEAR_GRADIENT:
        g.kind = GSHSCENE_FIXED_GRADIENT_LINEAR;
        memcpy(g.v, p->v, sizeof g.v);
        break;
      case GFNT_PAINT_RADIAL_GRADIENT:
        g.kind = GSHSCENE_FIXED_GRADIENT_RADIAL;
        memcpy(g.v, p->v, sizeof g.v);
        break;
      default:   // sweep: the angles are half turns at 14 bits, so the center is carried to 14 bits
        g.kind = GSHSCENE_FIXED_GRADIENT_SWEEP;
        g.bits = 14u;
        if (!gfnt_colr_times(p->v[0], 16384, &g.v[0]) || !gfnt_colr_times(p->v[1], 16384, &g.v[1])) {
          r = gfnt_error_set(l->error, GFNT_ERR_LIMIT, GFNT_COLR_TAG, 0, l->glyph,
              "a sweep's center past the scene's fixed range");
        }
        // COLR stores a sweep's angles biased: "add 1.0 and multiply by 180 degrees" gives counter-clockwise
        // degrees, so the half turns of fixed.h are the stored F2.14 plus one (16384).
        if (!gfnt_colr_plus(p->v[2], 16384, &g.v[2]) || !gfnt_colr_plus(p->v[3], 16384, &g.v[3])) {
          r = gfnt_error_set(l->error, GFNT_ERR_LIMIT, GFNT_COLR_TAG, 0, l->glyph,
              "a sweep's angle past the scene's fixed range");
        }
        break;
    }
  }
  if (r == GFNT_OK) {
    g.extend = (uint32_t)line.extend > 2u ? 0u : (uint32_t)line.extend;   // COLR: an unknown extend is pad
    g.primaries = GSHSCENE_FIXED_PRIMARIES_SRGB;
    g.transfer = encoded ? GSHSCENE_FIXED_TRANSFER_SRGB : GSHSCENE_FIXED_TRANSFER_LINEAR;
    g.premultiplied = encoded ? 0u : 1u;
    g.ops = t_count ? t : NULL;
    g.op_count = t_count;
    g.stop_count = line.stop_count;
    g.stops = stops;
    r = gfnt_colr_scene(l, gshscene_store_intern_gradient_fixed(l->store, l->frame, &g, out),
        "the scene refused a gradient");
  }
  if (stops) {
    l->face->allocator->free_fn(l->face->allocator->ctx, stops);
  }
  return r;
}

/**
 * Fill @p shape (a glyph's run, or the plane) with a leaf paint: its item map is the transforms @p ops, and a
 * gradient's own map is @p t (L4, L6).
 */
static GFNT_Result gfnt_colr_leaf(GFNT_ColrLower * l, const GFNT_ColorPaint * p, const GSHSCENE_Resource * shape,
    const GSHSCENE_Fixed_Op * ops, size_t op_count, const GSHSCENE_Fixed_Op * t, size_t t_count) {
  const bool plane = shape == l->plane;
  const GSHSCENE_Fixed_Op * item_ops = op_count ? ops : NULL;
  GFNT_Result r;

  if (p->format == GFNT_PAINT_SOLID) {
    GSHSCENE_Paint_Fixed paint = gfnt_colr_paint(p->palette_index, p->v[0]);

    return gfnt_colr_scene(l, plane
        ? gshscene_list_fill_plane_fixed(l->list, shape, l->frame, item_ops, op_count, GSHGEOM_FILL_NONZERO, &paint,
            l->run->layer)
        : gshscene_list_fill_fixed(l->list, shape, item_ops, op_count, GSHGEOM_FILL_NONZERO, &paint, l->run->layer),
        "the scene refused a fill");
  }
  const GSHSCENE_Resource * gradient = NULL;

  r = gfnt_colr_gradient(l, p, t, t_count, &gradient);
  if (r != GFNT_OK) {
    return r;
  }
  r = gfnt_colr_scene(l, plane
      ? gshscene_list_fill_plane_gradient_fixed(l->list, shape, l->frame, item_ops, op_count, GSHGEOM_FILL_NONZERO,
          gradient, GFNT_COLR_ONE, 14u, l->run->layer)
      : gshscene_list_fill_gradient_fixed(l->list, shape, item_ops, op_count, GSHGEOM_FILL_NONZERO, gradient,
          GFNT_COLR_ONE, 14u, l->run->layer),
      "the scene refused a gradient fill");
  gshscene_resource_release(gradient);   // the list holds its own
  return r;
}

/**
 * `PaintGlyph`: a fill of the glyph when its child reaches a solid or a gradient through transforms only (L4: the
 * transforms between them move the paint, not the outline), else a clip of the glyph around the child, which is a
 * bound for the paints beneath it (L6).
 */
static GFNT_Result gfnt_colr_glyph(GFNT_ColrLower * l, const GFNT_ColorPaint * p, size_t depth) {
  GFNT_ColorPaint c;
  GFNT_PaintRef ref = p->child;
  size_t d = depth + 1;
  GSHSCENE_Fixed_Op between[GSHSCENE_FIXED_MAX_OPS];
  size_t between_count = 0;
  const GSHSCENE_Resource * shape = NULL;
  GFNT_Result r;

  for (;;) {
    GSHSCENE_Fixed_Op op;
    bool transform = false;

    r = gfnt_colr_read(l, ref, d, &c);
    if (r == GFNT_OK) {
      r = gfnt_colr_make_op(l, &c, ref, &op, &transform);
    }
    if (r != GFNT_OK) {
      return r;
    }
    if (!transform) {
      break;
    }
    if (between_count == GSHSCENE_FIXED_MAX_OPS) {
      return gfnt_error_set(l->error, GFNT_ERR_LIMIT, GFNT_COLR_TAG, ref, l->glyph,
          "more transforms between a glyph and its paint than the scene's design map holds");
    }
    between[between_count++] = op;
    ref = c.child;
    ++d;
  }
  r = gfnt_colr_run_of(l, p->glyph, &shape);
  if (r != GFNT_OK) {
    return r;
  }
  if (gfnt_colr_is_leaf(c.format)) {
    r = gfnt_colr_leaf(l, &c, shape, l->ops, l->op_count, between, between_count);
    gshscene_resource_release(shape);
    return r;
  }
  r = gfnt_colr_scene(l, gshscene_list_clip_push_fixed(l->list, shape, l->op_count ? l->ops : NULL,
      l->op_count, GSHGEOM_FILL_NONZERO, l->run->layer), "the scene refused a glyph's clip");
  gshscene_resource_release(shape);
  if (r == GFNT_OK) {
    const bool was_bounded = l->bounded;
    const size_t was_ops = l->bound_ops;

    l->bounded = true;
    l->bound_ops = l->op_count;
    r = gfnt_colr_node(l, p->child, depth + 1);
    l->bounded = was_bounded;
    l->bound_ops = was_ops;
  }
  if (r == GFNT_OK) {
    r = gfnt_colr_scene(l, gshscene_list_clip_pop(l->list), "the scene refused a clip's pop");
  }
  return r;
}

/** The scene's operator for a `PaintComposite` mode: 0 to 12 by the table, 13 to 27 by identity, else clear (L7). */
static uint32_t gfnt_colr_composite_operator(uint32_t mode) {
  if (mode <= 12u) {
    return gfnt_colr_operator[mode];
  }
  return mode <= 27u ? mode : GFNT_COLR_UNKNOWN_MODE_OPERATOR;
}

/** `PaintComposite`: the backdrop and, composited onto it by the operator, the source, isolated together. */
static GFNT_Result gfnt_colr_composite(GFNT_ColrLower * l, const GFNT_ColorPaint * p, size_t depth) {
  GFNT_Result r = gfnt_colr_scene(l, gshscene_list_group_push_fixed(l->list, 0u, l->run->layer),
      "the scene refused a composite's group");
  if (r == GFNT_OK) {
    r = gfnt_colr_node(l, p->backdrop, depth + 1);
  }
  if (r == GFNT_OK) {
    r = gfnt_colr_scene(l, gshscene_list_group_push_fixed(l->list,
        gfnt_colr_composite_operator(p->composite_mode), l->run->layer), "the scene refused a source's group");
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
    case GFNT_PAINT_LINEAR_GRADIENT:
    case GFNT_PAINT_RADIAL_GRADIENT:
    case GFNT_PAINT_SWEEP_GRADIENT:
      // A paint no glyph bounds directly: it fills what the clips allow (L6), and the transforms above the innermost
      // clip are the gradient's own map.
      if (!l->bounded) {
        return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_UNBOUNDED, (uint32_t)p.format,
            "a paint no clip bounds: the font states no clip box for it");
      }
      r = gfnt_colr_plane(l);
      return r != GFNT_OK ? r : gfnt_colr_leaf(l, &p, l->plane, l->ops, l->bound_ops,
          l->ops + l->bound_ops, l->op_count - l->bound_ops);
    case GFNT_PAINT_GLYPH:
      return gfnt_colr_glyph(l, &p, depth);
    case GFNT_PAINT_COLR_GLYPH: {
      // The other glyph's graph, under the transforms above this node; where that glyph has a clip box it is pushed
      // here, under those transforms, and bounds the graph (L5: the reference renderers do the same for a box the
      // referenced glyph states, which Google's clip-box test glyphs exercise).
      GFNT_PaintRef root = 0;
      GFNT_ColorBox box;
      bool found = false;

      r = gfnt_face_color_base_paint(l->face, p.glyph, &root, l->error);
      if (r == GFNT_OK && root == 0) {
        return gfnt_colr_refuse(l, GFNT_COLOR_REFUSED_NO_RECORD, (uint32_t)p.format,
            "a color glyph naming a glyph with no version 1 record");
      }
      if (r == GFNT_OK) {
        r = gfnt_face_color_clip(l->face, p.glyph, &l->variation, &box, &found, l->error);
      }
      if (r == GFNT_OK && found) {
        r = gfnt_colr_scene(l, gshscene_list_clip_push_design_rect_fixed(l->list, l->frame,
            l->op_count ? l->ops : NULL, l->op_count, box.x_min, box.y_min, box.x_max, box.y_max, 0u,
            l->run->layer), "the scene refused a referenced glyph's clip box");
      }
      if (r == GFNT_OK) {
        const bool was_bounded = l->bounded;
        const size_t was_ops = l->bound_ops;

        if (found) {
          l->bounded = true;
          l->bound_ops = l->op_count;
        }
        r = gfnt_colr_node(l, root, depth + 1);
        l->bounded = was_bounded;
        l->bound_ops = was_ops;
      }
      if (r == GFNT_OK && found) {
        r = gfnt_colr_scene(l, gshscene_list_clip_pop(l->list), "the scene refused a clip's pop");
      }
      return r;
    }
    case GFNT_PAINT_COMPOSITE:
      return gfnt_colr_composite(l, &p, depth);
    default:
      break;
  }
  GSHSCENE_Fixed_Op op;
  bool transform = false;
  r = gfnt_colr_make_op(l, &p, ref, &op, &transform);
  if (r != GFNT_OK) {
    return r;
  }
  if (!transform) {
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

/** Version 0: fill @p glyph in the list's space, in place, with @p paint. */
static GFNT_Result gfnt_colr_fill(GFNT_ColrLower * l, uint32_t glyph, const GSHSCENE_Paint_Fixed * paint) {
  const GSHSCENE_Resource * shape = NULL;
  GFNT_Result r = gfnt_colr_run_of(l, glyph, &shape);

  if (r != GFNT_OK) {
    return r;
  }
  r = gfnt_colr_scene(l, gshscene_list_fill_fixed(l->list, shape, NULL, 0, GSHGEOM_FILL_NONZERO, paint,
      l->run->layer), "the scene refused a fill");
  gshscene_resource_release(shape);   // the list holds its own
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

/**
 * A version 1 glyph's graph: one isolated source-over group in the blend space COLRv1 composites in (L8; linear light
 * unless the run asks for the encoded space), around the clip of the glyph's `ClipList` box where it has one (L5), around
 * the graph (L2 to L7).
 */
static GFNT_Result gfnt_colr_graph(GFNT_ColrLower * l, GFNT_PaintRef root) {
  GFNT_ColorBox box;
  bool found = false;
  const bool encoded = l->run->space == GFNT_COLOR_SPACE_ENCODED;
  GFNT_Result r = gfnt_colr_run_of(l, l->glyph, &l->frame);

  if (r == GFNT_OK) {
    r = gfnt_face_color_clip(l->face, l->glyph, &l->variation, &box, &found, l->error);
  }
  if (r == GFNT_OK) {
    r = gfnt_colr_scene(l, gshscene_list_group_push_space_fixed(l->list, 0u, GSHSCENE_FIXED_PRIMARIES_SRGB,
        encoded ? GSHSCENE_FIXED_TRANSFER_SRGB : GSHSCENE_FIXED_TRANSFER_LINEAR, l->run->layer),
        "the scene refused the glyph's group");
  }
  if (r == GFNT_OK && found) {
    r = gfnt_colr_scene(l, gshscene_list_clip_push_design_rect_fixed(l->list, l->frame, NULL, 0, box.x_min, box.y_min,
        box.x_max, box.y_max, 0u, l->run->layer), "the scene refused the glyph's clip box");
    l->bounded = true;
    l->bound_ops = 0;
  }
  if (r == GFNT_OK) {
    r = gfnt_colr_node(l, root, 0);
  }
  if (r == GFNT_OK && found) {
    r = gfnt_colr_scene(l, gshscene_list_clip_pop(l->list), "the scene refused a clip's pop");
  }
  if (r == GFNT_OK) {
    r = gfnt_colr_scene(l, gshscene_list_group_pop(l->list), "the scene refused a group's pop");
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
  if (!gfnt_scene_instance_of(face, &l.variation)) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, glyph, "a face resource this library's provider does not bind");
  }
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
  r = root ? gfnt_colr_graph(&l, root) : gfnt_colr_layers(&l, layers);
  gshscene_resource_release(l.plane);
  gshscene_resource_release(l.frame);
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
