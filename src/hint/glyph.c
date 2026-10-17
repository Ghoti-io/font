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
 * Hinting one glyph, simple or composite.
 *
 * A simple glyph's points go into the glyph zone with four phantom points and its
 * own program runs over the face's context. A composite is assembled from its
 * components *after* each of them has been hinted, as the specification and
 * FreeType both do: a component is hinted by its own program, transformed, and
 * moved to its place (by an offset that may be rounded to the grid, or by matching
 * one of its points to one already placed), and then the composite's own program,
 * if it has one, runs over all the points at once with their hinted positions as
 * their originals.
 */

#include "vm_internal.h"

#include <string.h>

#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include "../var/gvar.h"
#include "../var/metvar.h"
#include "../glyf/glyf.h"
#include "../outline/outline.h"
#include "../core/chain.h"
#include "../sfnt/sfnt.h"

#define GFNT_HINT_MAX_COMPONENTS 1024u

/* A glyph as hinting leaves it: points in 26.6 pixels, and its four phantom points. */
typedef struct GFNT_HintedGlyph {
  const GFNT_Allocator * allocator;
  GFNT_HintPoint * points;
  uint8_t * on;
  size_t count;
  size_t capacity;
  size_t * ends;           /* last point of each contour */
  size_t end_count;
  size_t end_capacity;
  GFNT_HintPoint phantom[4];
  int32_t advance;         /* what a caller is told the advance is, 26.6 */
} GFNT_HintedGlyph;

/* What a walk over a glyph and its components accumulates. */
typedef struct GFNT_HintWalk {
  const GFNT_Face * face;
  const GFNT_HintContext * ctx;
  GFNT_Error * error;
  bool failed;             /* some program, or the context, could not run */
  bool ran_program;        /* some glyph's program ran to the end */
  GFNT_HintFault fault;    /* the first such failure */
  bool normalised;         /* the top glyph's program made a vector from a direction */
  uint8_t * touches;       /* diagnostics: the top simple glyph's touch bytes */
  size_t touch_capacity;
} GFNT_HintWalk;

/* To the nearest whole pixel, a tie going up: (v + 32) with the fraction cut off.
   Not the symmetric rounding of the rounding states; a negative half-pixel goes
   toward zero, as FreeType's does. */
static int32_t gfnt_hint_grid(int32_t v) {
  return (int32_t)((uint32_t)gfnt_hint_add(v, 32) & ~63u);
}

static bool gfnt_hint_ctx_compat(const GFNT_HintContext * ctx) {
  return ctx->params.version >= 40 && !(ctx->instruct_control & 4u);
}

static void gfnt_hinted_free(GFNT_HintedGlyph * g) {
  if (g->points) g->allocator->free_fn(g->allocator->ctx, g->points);
  if (g->on) g->allocator->free_fn(g->allocator->ctx, g->on);
  if (g->ends) g->allocator->free_fn(g->allocator->ctx, g->ends);
  memset(g, 0, sizeof *g);
}

static GFNT_Result gfnt_hinted_reserve(GFNT_HintedGlyph * g, size_t points,
    size_t contours) {
  const GFNT_Allocator * a = g->allocator;

  if (g->count + points > g->capacity) {
    size_t want = (g->count + points) * 2u + 8u;
    GFNT_HintPoint * p = a->realloc_fn(a->ctx, g->points, want * sizeof *p);
    uint8_t * o;

    if (!p) return GFNT_ERR_OOM;
    g->points = p;
    o = a->realloc_fn(a->ctx, g->on, want);
    if (!o) return GFNT_ERR_OOM;
    g->on = o;
    g->capacity = want;
  }
  if (g->end_count + contours > g->end_capacity) {
    size_t want = (g->end_count + contours) * 2u + 4u;
    size_t * e = a->realloc_fn(a->ctx, g->ends, want * sizeof *e);

    if (!e) return GFNT_ERR_OOM;
    g->ends = e;
    g->end_capacity = want;
  }
  return GFNT_OK;
}

/* Scaled font units to 26.6, with the one rounding the context's scale gives. */
static int32_t gfnt_hint_scaled(const GFNT_HintContext * ctx, int32_t units) {
  return gfnt_hint_mul_fix(units, ctx->scale);
}

/* The metrics a glyph's phantom points come from: its box edge, bearing, advance. */
static GFNT_Result gfnt_hint_metrics(const GFNT_Face * face, uint32_t glyph,
    int32_t * x_min, int32_t * bearing, int32_t * advance, GFNT_Error * error) {
  GFNT_Box box;
  GFNT_Result r = gfnt_face_glyph_stated_box(face, glyph, &box, error);

  if (r != GFNT_OK) return r;
  *x_min = gfnt_box_is_empty(&box) ? 0 : box.x_min / 64;
  r = gfnt_face_glyph_advance(face, glyph, NULL, advance, error);
  if (r == GFNT_OK) r = gfnt_face_glyph_side_bearing(face, glyph, NULL, bearing, error);
  return r;
}

/* A `gvar` delta (font units with GFNT_GVAR_FRACTION_BITS fractional bits) to 16.16
   font units, rounded half up. */
static int32_t gfnt_hint_delta_fixed(int64_t delta) {
  int64_t v = (delta + ((int64_t)1 << (GFNT_GVAR_FRACTION_BITS - 17))) >> (GFNT_GVAR_FRACTION_BITS - 16);

  return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

/* Whole font units from 16.16, rounded half up: -3.5 is -3. */
static int32_t gfnt_hint_fixed_units(int32_t v) {
  return (int32_t)(((int64_t)v + 0x8000) >> 16);
}

/* The bias, in 26.6 units of 16.16 (so 0x240 is 0.0088 of one 26.6 unit), that a scaled
   position of a moved point carries before it is rounded. It is not in any specification:
   the reference's positions for a variable instance behave as if rounded with it, which
   was fitted to 57,802 points of random instances and agrees on all but 0.12% of them
   (those are off by one 26.6 unit, at a value within a few thousandths of a half). */
#define GFNT_HINT_VARIATION_BIAS 0x240

/* A position in font units with a 16.16 delta, to 26.6 pixels: the sum is scaled and not
   its rounded parts, as the reference does for a variable font's moved points. */
static int32_t gfnt_hint_scaled_fixed(const GFNT_HintContext * ctx, int32_t units, int32_t delta16) {
  int64_t u16 = ((int64_t)units << 16) + delta16;
  int64_t v = (u16 * ctx->scale + ((int64_t)1 << 31) + ((int64_t)GFNT_HINT_VARIATION_BIAS << 16)) >> 32;

  return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

/* How far the first two phantom points move at the context's location, 16.16 font units.
   A face whose `HVAR` states an advance variation takes it from there; otherwise `gvar`
   carries the phantom points' deltas with the glyph's own. @p gvar_left and @p gvar_right
   are those, and are what stand when `HVAR` has nothing to say. */
static void gfnt_hint_phantom_moves(const GFNT_Face * face, const GFNT_HintContext * ctx,
    uint32_t glyph, int32_t gvar_left, int32_t gvar_right, int32_t * left, int32_t * right) {
  const GFNT_Variation * v = gfnt_hint_ctx_variation(ctx);
  int64_t advance = 0;
  int64_t bearing = 0;
  GFNT_Error scratch;

  *left = gvar_left;
  *right = gvar_right;
  if (!v || !gfnt_face_has_table(face, GFNT_TAG_HVAR)) return;
  if (gfnt_hvar_delta(face, glyph, GFNT_HVAR_ADVANCE, v->coords, v->count, &advance, &scratch)
      != GFNT_OK) {
    return;
  }
  *left = 0;
  if (gfnt_hvar_delta(face, glyph, GFNT_HVAR_LEFT_BEARING, v->coords, v->count, &bearing,
      &scratch) == GFNT_OK) {
    *left = -gfnt_hint_delta_fixed(bearing);
  }
  *right = *left + gfnt_hint_delta_fixed(advance);
}

/* What `gvar` does to a simple glyph at the context's location, in 16.16 font units:
   @p dx and @p dy each hold the glyph's points and then its four phantom points. Zero
   everywhere when the context is not at a location or the face has no `gvar`. */
static GFNT_Result gfnt_hint_vary(const GFNT_Face * face, const GFNT_HintContext * ctx,
    uint32_t glyph, size_t count, const int32_t * x, const int32_t * y,
    const size_t * ends, size_t contours, int32_t * dx, int32_t * dy, GFNT_Error * error) {
  const GFNT_Allocator * a = face->allocator;
  const GFNT_Variation * variation = gfnt_hint_ctx_variation(ctx);
  GFNT_GvarPoints points = {0};
  size_t total = count + GFNT_GVAR_PHANTOM_POINTS;
  int64_t * wide;
  size_t * past;
  GFNT_Result r;

  memset(dx, 0, total * sizeof *dx);
  memset(dy, 0, total * sizeof *dy);
  if (!variation || !gfnt_face_has_table(face, GFNT_TAG_GVAR)) return GFNT_OK;
  wide = a->calloc_fn(a->ctx, total * 2u, sizeof *wide);
  past = a->calloc_fn(a->ctx, contours ? contours : 1u, sizeof *past);
  if (!wide || !past) {
    if (wide) a->free_fn(a->ctx, wide);
    if (past) a->free_fn(a->ctx, past);
    return gfnt_error_set(error, GFNT_ERR_OOM, GFNT_TAG_GVAR, 0, glyph,
        "no memory for a glyph's variation deltas");
  }
  for (size_t c = 0; c < contours; ++c) past[c] = ends[c] + 1u;
  points.count = count;
  points.x = x;
  points.y = y;
  points.contour_ends = past;
  points.contour_count = contours;
  points.fixed_interpolation = true;
  r = gfnt_gvar_glyph_deltas(face, glyph, variation->coords, variation->count, &points,
      wide, wide + total, error);
  if (r == GFNT_OK) {
    for (size_t i = 0; i < total; ++i) {
      dx[i] = gfnt_hint_delta_fixed(wide[i]);
      dy[i] = gfnt_hint_delta_fixed(wide[total + i]);
    }
  }
  a->free_fn(a->ctx, wide);
  a->free_fn(a->ctx, past);
  return r;
}

static void gfnt_hint_set_phantoms(const GFNT_HintContext * ctx, int32_t x_min,
    int32_t bearing, int32_t advance, int32_t d0, int32_t d1, GFNT_HintPoint pp[4]) {
  bool varied = gfnt_hint_ctx_variation(ctx) != NULL;

  pp[0].x = varied ? gfnt_hint_scaled_fixed(ctx, x_min - bearing, d0) : gfnt_hint_scaled(ctx, x_min - bearing);
  pp[0].y = 0;
  pp[1].x = varied ? gfnt_hint_scaled_fixed(ctx, x_min - bearing + advance, d1)
               : gfnt_hint_scaled(ctx, x_min - bearing + advance);
  pp[1].y = 0;
  pp[2].x = pp[2].y = 0;
  pp[3].x = pp[3].y = 0;
}

/* The advance a caller is told: the distance between the first two phantom points
   as the program left them, to the whole pixel. */
static int32_t gfnt_hint_reported_advance(const GFNT_HintPoint pp[4]) {
  return gfnt_hint_grid(gfnt_hint_sub(pp[1].x, pp[0].x));
}

/* A simple glyph: load it, run its program (even an empty one), read it back. */
static GFNT_Result gfnt_hint_simple(GFNT_HintWalk * w, uint32_t glyph, bool top,
    GFNT_HintedGlyph * out) {
  const GFNT_HintContext * ctx = w->ctx;
  const GFNT_Allocator * a = w->face->allocator;
  GFNT_Outline * units = NULL;
  const uint8_t * program = NULL;
  size_t program_length = 0;
  GFNT_HintRun * run = NULL;
  GFNT_HintGlyphInput input;
  int32_t * x = NULL;
  int32_t * y = NULL;
  int32_t * org_x = NULL;      /* scaled originals where a variable font's deltas are fractional */
  int32_t * org_y = NULL;
  uint8_t * on = NULL;
  size_t * ends = NULL;
  size_t n;
  int32_t x_min = 0;
  int32_t bearing = 0;
  int32_t advance = 0;
  int32_t pd0 = 0;             /* the first two phantom points' 16.16 deltas */
  int32_t pd1 = 0;
  bool hinted = false;
  GFNT_Result r;

  r = gfnt_face_glyph_outline(w->face, glyph, NULL, NULL, &units, w->error);
  if (r != GFNT_OK) return r;
  r = gfnt_glyf_program(w->face, glyph, &program, &program_length, w->error);
  if (r == GFNT_OK) r = gfnt_hint_metrics(w->face, glyph, &x_min, &bearing, &advance, w->error);
  if (r != GFNT_OK) {
    gfnt_outline_destroy(units);
    return r;
  }
  n = units->point_count + 4u;
  out->allocator = a;
  r = gfnt_hinted_reserve(out, units->point_count + 4u, units->contour_count);
  x = a->calloc_fn(a->ctx, n, sizeof *x);
  y = a->calloc_fn(a->ctx, n, sizeof *y);
  on = a->calloc_fn(a->ctx, n, 1);
  ends = a->calloc_fn(a->ctx, units->contour_count ? units->contour_count : 1u, sizeof *ends);
  if (r != GFNT_OK || !x || !y || !on || !ends) {
    r = GFNT_ERR_OOM;
    goto done;
  }
  for (size_t i = 0; i < units->point_count; ++i) {
    x[i] = units->points[i].x / 64;
    y[i] = units->points[i].y / 64;
    on[i] = units->tags[i] == GFNT_POINT_ON;
  }
  for (size_t c = 0; c < units->contour_count; ++c) {
    ends[c] = units->contours[c] - 1u;
  }
  x[n - 4u] = x_min - bearing;
  x[n - 3u] = x_min - bearing + advance;
  for (size_t i = n - 4u; i < n; ++i) on[i] = 1;
  if (gfnt_hint_ctx_variation(ctx)) {
    int32_t * dx = a->calloc_fn(a->ctx, n * 2u, sizeof *dx);

    org_x = a->calloc_fn(a->ctx, n, sizeof *org_x);
    org_y = a->calloc_fn(a->ctx, n, sizeof *org_y);
    if (!dx || !org_x || !org_y) {
      if (dx) a->free_fn(a->ctx, dx);
      r = GFNT_ERR_OOM;
      goto done;
    }
    r = gfnt_hint_vary(w->face, ctx, glyph, units->point_count, x, y, ends,
        units->contour_count, dx, dx + n, w->error);
    if (r == GFNT_OK) {
      gfnt_hint_phantom_moves(w->face, ctx, glyph, dx[n - 4u], dx[n - 3u], &pd0, &pd1);
      dx[n - 4u] = pd0;
      dx[n - 3u] = pd1;
      for (size_t i = 0; i < n; ++i) {
        int32_t d_x = dx[i];
        int32_t d_y = dx[n + i];

        /* Only the first two phantom points have a horizontal delta here; the vertical
           ones are not derived from vertical metrics. */
        if (i >= n - 2u) d_x = d_y = 0;
        org_x[i] = gfnt_hint_scaled_fixed(ctx, x[i], d_x);
        org_y[i] = gfnt_hint_scaled_fixed(ctx, y[i], d_y);
        x[i] += gfnt_hint_fixed_units(d_x);
        y[i] += gfnt_hint_fixed_units(d_y);
      }
    }
    a->free_fn(a->ctx, dx);
    if (r != GFNT_OK) goto done;
  }

  /* Program present or not, the glyph goes through the same run, since that is
     where the phantom points are placed and the origin is found. */
  if (gfnt_hint_ctx_usable(ctx) && !(ctx->instruct_control & 1u)) {
    input.point_count = n;
    input.x = x;
    input.y = y;
    input.on_curve = on;
    input.contour_count = units->contour_count;
    input.ends = ends;
    input.pixels = false;
    input.org_x = org_x;
    input.org_y = org_y;
    r = gfnt_hint_run_glyph(ctx, &input, program, program_length, &run);
    if (r == GFNT_ERR_OOM) goto done;
    if (r == GFNT_OK) {
      hinted = true;
      if (program_length > 0) w->ran_program = true;
    }
    else if (!w->failed) {
      w->failed = true;
      w->fault = run ? gfnt_hint_run_fault(run) : w->fault;
    }
  }
  else if (!gfnt_hint_ctx_usable(ctx) && !w->failed) {
    w->failed = true;
    w->fault = gfnt_hint_ctx_fault(ctx);
  }
  for (size_t i = 0; i < units->point_count; ++i) {
    GFNT_HintPoint p;

    if (hinted) {
      int32_t px = 0;
      int32_t py = 0;

      gfnt_hint_run_point(run, i, &px, &py);
      p.x = px;
      p.y = py;
      out->on[out->count] = gfnt_hint_run_on_curve(run, i);
    }
    else {
      p.x = gfnt_hint_scaled(ctx, x[i]);
      p.y = gfnt_hint_scaled(ctx, y[i]);
      out->on[out->count] = on[i];
    }
    out->points[out->count++] = p;
  }
  for (size_t c = 0; c < units->contour_count; ++c) {
    out->ends[out->end_count++] = out->count - units->point_count + ends[c];
  }
  if (hinted) {
    for (size_t i = 0; i < 4; ++i) {
      gfnt_hint_run_point(run, n - 4u + i, &out->phantom[i].x, &out->phantom[i].y);
    }
    out->advance = gfnt_hint_reported_advance(out->phantom);
    w->normalised = w->normalised || gfnt_hint_run_normalised(run);
    if (top) {
      for (size_t i = 0; w->touches && i < w->touch_capacity && i < units->point_count; ++i) {
        w->touches[i] = gfnt_hint_run_touch(run, i);
      }
    }
  }
  else {
    gfnt_hint_set_phantoms(ctx, x_min, bearing, advance, pd0, pd1, out->phantom);
    out->advance = gfnt_hint_grid(gfnt_hint_sub(out->phantom[1].x, out->phantom[0].x));
  }
  r = GFNT_OK;

done:
  gfnt_hint_run_destroy(run);
  gfnt_outline_destroy(units);
  if (x) a->free_fn(a->ctx, x);
  if (y) a->free_fn(a->ctx, y);
  if (org_x) a->free_fn(a->ctx, org_x);
  if (org_y) a->free_fn(a->ctx, org_y);
  if (on) a->free_fn(a->ctx, on);
  if (ends) a->free_fn(a->ctx, ends);
  return r;
}

static int64_t gfnt_hint_isqrt64(uint64_t n) {
  uint64_t root = 0;
  uint64_t bit = (uint64_t)1 << 62;

  while (bit > n) bit >>= 2;
  while (bit) {
    if (n >= root + bit) {
      n -= root + bit;
      root = (root >> 1) + bit;
    }
    else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return (int64_t)root;
}

static GFNT_Result gfnt_hint_glyph_rec(GFNT_HintWalk * w, uint32_t glyph, size_t depth,
    const GFNT_GlyphChain * chain, bool top, GFNT_HintedGlyph * out);

/* A composite: hint each component, place it, then run the composite's own program. */
static GFNT_Result gfnt_hint_composite_body(GFNT_HintWalk * w, uint32_t glyph, size_t depth,
    const GFNT_GlyphChain * chain, bool top, GFNT_HintedGlyph * out, GFNT_GlyfComponent * components,
    size_t component_count) {
  const GFNT_HintContext * ctx = w->ctx;
  const GFNT_Allocator * a = w->face->allocator;
  GFNT_GlyphChain here;
  const uint8_t * program = NULL;
  size_t program_length = 0;
  int32_t x_min = 0;
  int32_t bearing = 0;
  int32_t advance = 0;
  bool borrowed = false;           /* USE_MY_METRICS supplied the phantom points */
  GFNT_HintPoint borrowed_pp[4];
  int32_t borrowed_advance = 0;
  int32_t pd0 = 0;
  int32_t pd1 = 0;
  GFNT_Result r;

  (void)top;
  r = gfnt_glyf_program(w->face, glyph, &program, &program_length, w->error);
  if (r == GFNT_OK) r = gfnt_hint_metrics(w->face, glyph, &x_min, &bearing, &advance, w->error);
  if (r != GFNT_OK) return r;
  here.parent = chain;
  here.glyph = glyph;
  out->allocator = a;
  if (gfnt_hint_ctx_variation(ctx) && gfnt_face_has_table(w->face, GFNT_TAG_GVAR)) {
    /* gvar moves a composite's component offsets, and its first two phantom points. */
    size_t total = component_count + GFNT_GVAR_PHANTOM_POINTS;
    int64_t * wide = a->calloc_fn(a->ctx, total * 2u, sizeof *wide);
    GFNT_GvarPoints points = {0};
    const GFNT_Variation * v = gfnt_hint_ctx_variation(ctx);

    if (!wide) return GFNT_ERR_OOM;
    points.count = component_count;
    r = gfnt_gvar_glyph_deltas(w->face, glyph, v->coords, v->count, &points, wide, wide + total,
        w->error);
    if (r != GFNT_OK) {
      a->free_fn(a->ctx, wide);
      return r;
    }
    for (size_t k = 0; k < component_count; ++k) {
      if (components[k].flags & GFNT_GLYF_ARGS_ARE_XY) {
        components[k].delta_x = gfnt_hint_delta_fixed(wide[k]);
        components[k].delta_y = gfnt_hint_delta_fixed(wide[total + k]);
      }
    }
    gfnt_hint_phantom_moves(w->face, ctx, glyph, gfnt_hint_delta_fixed(wide[component_count]),
        gfnt_hint_delta_fixed(wide[component_count + 1u]), &pd0, &pd1);
    a->free_fn(a->ctx, wide);
  }

  for (size_t k = 0; k < component_count; ++k) {
    const GFNT_GlyfComponent * c = &components[k];
    GFNT_HintedGlyph part;
    int32_t dx;
    int32_t dy;
    bool transformed = c->xx != 0x10000 || c->xy != 0 || c->yx != 0 || c->yy != 0x10000;

    memset(&part, 0, sizeof part);
    part.allocator = a;
    if (gfnt_glyph_chain_has(&here, c->glyph)) {
      return gfnt_error_set(w->error, GFNT_ERR_CORRUPT, GFNT_TAG('g', 'l', 'y', 'f'), 0, glyph,
          "a composite glyph includes itself as a component, directly or through another glyph");
    }
    r = gfnt_hint_glyph_rec(w, c->glyph, depth + 1u, &here, false, &part);
    if (r != GFNT_OK) {
      gfnt_hinted_free(&part);
      return r;
    }
    if (transformed) {
      for (size_t i = 0; i < part.count; ++i) {
        int32_t px = part.points[i].x;
        int32_t py = part.points[i].y;

        part.points[i].x = gfnt_hint_add(gfnt_hint_mul_fix(px, c->xx), gfnt_hint_mul_fix(py, c->xy));
        part.points[i].y = gfnt_hint_add(gfnt_hint_mul_fix(px, c->yx), gfnt_hint_mul_fix(py, c->yy));
      }
    }
    if (c->flags & GFNT_GLYF_ARGS_ARE_XY) {
      /* gvar moves the offset in whole font units, rounded half up. */
      int32_t ox = c->arg1 + gfnt_hint_fixed_units(c->delta_x);
      int32_t oy = c->arg2 + gfnt_hint_fixed_units(c->delta_y);

      /* A scaled offset is scaled by the component's transform in font units, and
         only then to pixels: the two roundings happen in that order. */
      if ((c->flags & GFNT_GLYF_SCALED_OFFSET) && !(c->flags & GFNT_GLYF_UNSCALED_OFFSET)
          && transformed) {
        int64_t sx = gfnt_hint_isqrt64((uint64_t)((int64_t)c->xx * c->xx + (int64_t)c->xy * c->xy));
        int64_t sy = gfnt_hint_isqrt64((uint64_t)((int64_t)c->yy * c->yy + (int64_t)c->yx * c->yx));

        ox = gfnt_hint_mul_fix(ox, (int32_t)sx);
        oy = gfnt_hint_mul_fix(oy, (int32_t)sy);
      }
      dx = gfnt_hint_scaled(ctx, ox);
      dy = gfnt_hint_scaled(ctx, oy);
      if (c->flags & GFNT_GLYF_ROUND_XY_TO_GRID) {
        if (!gfnt_hint_ctx_compat(ctx)) dx = gfnt_hint_grid(dx);
        dy = gfnt_hint_grid(dy);
      }
    }
    else {
      if (c->arg1 < 0 || (size_t)c->arg1 >= out->count || c->arg2 < 0
          || (size_t)c->arg2 >= part.count) {
        gfnt_hinted_free(&part);
        return gfnt_error_set(w->error, GFNT_ERR_CORRUPT, GFNT_TAG('g', 'l', 'y', 'f'), 0, glyph,
            "a component matches a point neither glyph has");
      }
      dx = gfnt_hint_sub(out->points[c->arg1].x, part.points[c->arg2].x);
      dy = gfnt_hint_sub(out->points[c->arg1].y, part.points[c->arg2].y);
    }
    r = gfnt_hinted_reserve(out, part.count, part.end_count);
    if (r != GFNT_OK) {
      gfnt_hinted_free(&part);
      return r;
    }
    for (size_t i = 0; i < part.count; ++i) {
      out->points[out->count + i].x = gfnt_hint_add(part.points[i].x, dx);
      out->points[out->count + i].y = gfnt_hint_add(part.points[i].y, dy);
      out->on[out->count + i] = part.on[i];
    }
    for (size_t e = 0; e < part.end_count; ++e) {
      out->ends[out->end_count + e] = out->count + part.ends[e];
    }
    out->count += part.count;
    out->end_count += part.end_count;
    if (c->flags & GFNT_GLYF_USE_MY_METRICS) {
      borrowed = true;
      memcpy(borrowed_pp, part.phantom, sizeof borrowed_pp);
      borrowed_advance = part.advance;
    }
    gfnt_hinted_free(&part);
  }

  if (borrowed) {
    memcpy(out->phantom, borrowed_pp, sizeof borrowed_pp);
  }
  else {
    gfnt_hint_set_phantoms(ctx, x_min, bearing, advance, pd0, pd1, out->phantom);
  }
  out->advance = borrowed ? borrowed_advance
      : gfnt_hint_grid(gfnt_hint_sub(out->phantom[1].x, out->phantom[0].x));

  if (program_length > 0 && gfnt_hint_ctx_usable(ctx) && !(ctx->instruct_control & 1u)) {
    GFNT_HintGlyphInput input;
    GFNT_HintRun * run = NULL;
    size_t n = out->count + 4u;
    int32_t * x = a->calloc_fn(a->ctx, n, sizeof *x);
    int32_t * y = a->calloc_fn(a->ctx, n, sizeof *y);
    uint8_t * on = a->calloc_fn(a->ctx, n, 1);

    if (!x || !y || !on) {
      if (x) a->free_fn(a->ctx, x);
      if (y) a->free_fn(a->ctx, y);
      if (on) a->free_fn(a->ctx, on);
      return GFNT_ERR_OOM;
    }
    for (size_t i = 0; i < out->count; ++i) {
      x[i] = out->points[i].x;
      y[i] = out->points[i].y;
      on[i] = out->on[i];
    }
    for (size_t i = 0; i < 4; ++i) {
      x[out->count + i] = out->phantom[i].x;
      y[out->count + i] = out->phantom[i].y;
      on[out->count + i] = 1;
    }
    input.point_count = n;
    input.x = x;
    input.y = y;
    input.on_curve = on;
    input.contour_count = out->end_count;
    input.ends = out->ends;
    input.pixels = true;
    input.org_x = input.org_y = NULL;
    r = gfnt_hint_run_glyph(ctx, &input, program, program_length, &run);
    if (r == GFNT_OK) {
      for (size_t i = 0; i < out->count; ++i) {
        gfnt_hint_run_point(run, i, &out->points[i].x, &out->points[i].y);
        out->on[i] = gfnt_hint_run_on_curve(run, i);
      }
      for (size_t i = 0; i < 4; ++i) {
        gfnt_hint_run_point(run, out->count + i, &out->phantom[i].x, &out->phantom[i].y);
      }
      out->advance = gfnt_hint_reported_advance(out->phantom);
      w->ran_program = true;
      w->normalised = w->normalised || gfnt_hint_run_normalised(run);
    }
    else if (r != GFNT_ERR_OOM && !w->failed) {
      w->failed = true;
      w->fault = run ? gfnt_hint_run_fault(run) : w->fault;
      r = GFNT_OK;
    }
    else if (r != GFNT_ERR_OOM) {
      r = GFNT_OK;
    }
    gfnt_hint_run_destroy(run);
    a->free_fn(a->ctx, x);
    a->free_fn(a->ctx, y);
    a->free_fn(a->ctx, on);
    return r;
  }
  return GFNT_OK;
}


static GFNT_Result gfnt_hint_composite(GFNT_HintWalk * w, uint32_t glyph, size_t depth,
    const GFNT_GlyphChain * chain, bool top, GFNT_HintedGlyph * out) {
  const GFNT_Allocator * a = w->face->allocator;
  GFNT_GlyfComponent * components = NULL;
  size_t component_count = 0;
  GFNT_Result r;

  r = gfnt_glyf_components(w->face, glyph, NULL, 0, &component_count, w->error);
  if (r == GFNT_OK && component_count > GFNT_HINT_MAX_COMPONENTS) {
    r = gfnt_error_set(w->error, GFNT_ERR_LIMIT, GFNT_TAG('g', 'l', 'y', 'f'), 0, glyph,
        "a composite has more components than hinting will place");
  }
  if (r == GFNT_OK) {
    components = a->calloc_fn(a->ctx, component_count ? component_count : 1u, sizeof *components);
    if (!components) r = GFNT_ERR_OOM;
  }
  if (r == GFNT_OK) {
    r = gfnt_glyf_components(w->face, glyph, components, component_count, &component_count, w->error);
  }
  if (r == GFNT_OK) {
    r = gfnt_hint_composite_body(w, glyph, depth, chain, top, out, components, component_count);
  }
  if (components) a->free_fn(a->ctx, components);
  return r;
}

static GFNT_Result gfnt_hint_glyph_rec(GFNT_HintWalk * w, uint32_t glyph, size_t depth,
    const GFNT_GlyphChain * chain, bool top, GFNT_HintedGlyph * out) {
  bool composite = false;
  GFNT_Result r;

  if (depth > w->face->limits.max_composite_depth) {
    return gfnt_error_set(w->error, GFNT_ERR_LIMIT, GFNT_TAG('g', 'l', 'y', 'f'), 0, glyph,
        "composite glyphs nest deeper than max_composite_depth allows");
  }
  r = gfnt_glyf_is_composite(w->face, glyph, &composite, w->error);
  if (r != GFNT_OK) return r;
  return composite ? gfnt_hint_composite(w, glyph, depth, chain, top, out)
                   : gfnt_hint_simple(w, glyph, top, out);
}

GFNT_Result gfnt_hint_glyph_outline_in(const GFNT_Face * face,
    const GFNT_HintContext * ctx, uint32_t glyph, const GFNT_Allocator * out_allocator,
    GFNT_Outline ** out, GFNT_HintStatus * status, GFNT_HintFault * fault,
    int32_t * advance_out, uint8_t * touches, size_t touch_capacity, bool * normalised,
    GFNT_Error * error) {
  GFNT_HintWalk walk;
  GFNT_HintedGlyph g;
  GFNT_Outline * result = NULL;
  GFNT_Result r;

  gfnt_error_clear(error);
  if (!face || !ctx || !out || !status) return GFNT_ERR_INVALID;
  *out = NULL;
  if (advance_out) *advance_out = 0;
  if (fault) memset(fault, 0, sizeof *fault);
  if (normalised) *normalised = false;
  memset(&walk, 0, sizeof walk);
  walk.face = face;
  walk.ctx = ctx;
  walk.error = error;
  walk.touches = touches;
  walk.touch_capacity = touch_capacity;
  memset(&g, 0, sizeof g);
  g.allocator = face->allocator;

  r = gfnt_hint_glyph_rec(&walk, glyph, 0, NULL, true, &g);
  if (r != GFNT_OK) {
    gfnt_hinted_free(&g);
    return r;
  }
  r = gfnt_outline_create(out_allocator ? out_allocator : face->allocator, &result, error);
  if (r == GFNT_OK) {
    size_t first = 0;
    int32_t origin = g.phantom[0].x;

    for (size_t c = 0; c < g.end_count && r == GFNT_OK; ++c) {
      r = gfnt_outline_begin_contour(result, error);
      for (size_t i = first; r == GFNT_OK && i <= g.ends[c]; ++i) {
        GFNT_Point p;

        p.x = gfnt_hint_sub(g.points[i].x, origin);
        p.y = g.points[i].y;
        r = gfnt_outline_add_point(result, p, g.on[i] ? GFNT_POINT_ON : GFNT_POINT_QUAD, error);
      }
      first = g.ends[c] + 1u;
    }
  }
  if (r == GFNT_OK) {
    result->space = GFNT_OUTLINE_PIXELS;
    *out = result;
    result = NULL;
    *status = walk.failed ? GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED
        : walk.ran_program ? GFNT_HINT_STATUS_HINTED : GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM;
    if (advance_out) *advance_out = g.advance;
    if (fault) *fault = walk.fault;
    if (normalised) *normalised = walk.normalised;
  }
  gfnt_outline_destroy(result);
  gfnt_hinted_free(&g);
  return r;
}

GFNT_Result gfnt_hint_glyph_outline(const GFNT_Face * face,
    const GFNT_HintContext * ctx, uint32_t glyph, GFNT_Outline ** out,
    GFNT_HintStatus * status, GFNT_HintFault * fault, int32_t * advance_out,
    uint8_t * touches, size_t touch_capacity, bool * normalised, GFNT_Error * error) {
  return gfnt_hint_glyph_outline_in(face, ctx, glyph, NULL, out, status, fault,
      advance_out, touches, touch_capacity, normalised, error);
}
