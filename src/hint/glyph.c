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
 * Hinting one glyph: its points go into the glyph zone with the four phantom
 * points, its own program runs over the face's context, and what the program
 * left is the hinted outline, in pixels.
 */

#include "vm_internal.h"

#include <string.h>

#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include "../glyf/glyf.h"
#include "../outline/outline.h"
#include "../sfnt/sfnt.h"

static GFNT_Result gfnt_hint_unhinted(const GFNT_HintContext * ctx,
    const GFNT_Outline * units, GFNT_Outline ** out, GFNT_Error * error) {
  GFNT_Outline * result = NULL;
  GFNT_Result r = gfnt_outline_create(units->allocator, &result, error);

  if (r != GFNT_OK) {
    return r;
  }
  for (size_t c = 0; c < units->contour_count; ++c) {
    size_t first = c == 0 ? 0 : units->contours[c - 1];
    r = gfnt_outline_begin_contour(result, error);
    for (size_t i = first; r == GFNT_OK && i < units->contours[c]; ++i) {
      GFNT_Point p;
      p.x = gfnt_hint_mul_fix(units->points[i].x / 64, ctx->scale);
      p.y = gfnt_hint_mul_fix(units->points[i].y / 64, ctx->scale);
      r = gfnt_outline_add_point(result, p, (GFNT_PointTag)units->tags[i], error);
    }
    if (r != GFNT_OK) {
      gfnt_outline_destroy(result);
      return r;
    }
  }
  result->space = GFNT_OUTLINE_PIXELS;
  *out = result;
  return GFNT_OK;
}

GFNT_Result gfnt_hint_glyph_outline(const GFNT_Face * face,
    const GFNT_HintContext * ctx, uint32_t glyph, GFNT_Outline ** out,
    GFNT_HintStatus * status, GFNT_HintFault * fault, int32_t * advance_out,
    uint8_t * touches, size_t touch_capacity, bool * normalised, GFNT_Error * error) {
  const GFNT_Allocator * a = face->allocator;
  GFNT_Outline * units = NULL;
  GFNT_Outline * hinted = NULL;
  const uint8_t * program = NULL;
  size_t program_length = 0;
  GFNT_HintRun * run = NULL;
  GFNT_HintGlyphInput input;
  int32_t * x = NULL;
  int32_t * y = NULL;
  uint8_t * on = NULL;
  size_t * ends = NULL;
  size_t n;
  GFNT_Box box;
  int32_t advance = 0;
  int32_t bearing = 0;
  bool composite = false;
  GFNT_Result r;

  gfnt_error_clear(error);
  if (!face || !ctx || !out || !status) {
    return GFNT_ERR_INVALID;
  }
  *out = NULL;
  if (advance_out) {
    *advance_out = 0;
  }
  if (fault) {
    memset(fault, 0, sizeof *fault);
  }
  if (normalised) {
    *normalised = false;
  }
  r = gfnt_face_glyph_outline(face, glyph, NULL, NULL, &units, error);
  if (r != GFNT_OK) {
    return r;
  }
  r = gfnt_glyf_is_composite(face, glyph, &composite, error);
  if (r == GFNT_OK && !composite) {
    r = gfnt_glyf_program(face, glyph, &program, &program_length, error);
  }
  if (r != GFNT_OK) {
    gfnt_outline_destroy(units);
    return r;
  }
  if (advance_out) {
    int32_t units_advance = 0;

    if (gfnt_face_glyph_advance(face, glyph, NULL, &units_advance, error) == GFNT_OK) {
      *advance_out = gfnt_hint_mul_fix(units_advance, ctx->scale);
    }
  }
  if (composite || program_length == 0 || !gfnt_hint_ctx_usable(ctx)) {
    *status = composite ? GFNT_HINT_STATUS_UNHINTED_COMPOSITE
        : !gfnt_hint_ctx_usable(ctx) ? GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED
        : GFNT_HINT_STATUS_UNHINTED_NO_PROGRAM;
    r = gfnt_hint_unhinted(ctx, units, out, error);
    gfnt_outline_destroy(units);
    return r;
  }

  n = units->point_count + 4u;
  x = a->calloc_fn(a->ctx, n, sizeof *x);
  y = a->calloc_fn(a->ctx, n, sizeof *y);
  on = a->calloc_fn(a->ctx, n, 1);
  ends = a->calloc_fn(a->ctx, units->contour_count ? units->contour_count : 1u, sizeof *ends);
  if (!x || !y || !on || !ends) {
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
  r = gfnt_face_glyph_stated_box(face, glyph, &box, error);
  if (r == GFNT_OK) r = gfnt_face_glyph_advance(face, glyph, NULL, &advance, error);
  if (r == GFNT_OK) r = gfnt_face_glyph_side_bearing(face, glyph, NULL, &bearing, error);
  if (r != GFNT_OK) {
    goto done;
  }
  /* Phantom points: the origin and the advance on the baseline, then the
     vertical pair (not yet derived from the vertical metrics). */
  x[n - 4u] = box.x_min / 64 - bearing;
  y[n - 4u] = 0;
  x[n - 3u] = x[n - 4u] + advance;
  y[n - 3u] = 0;
  x[n - 2u] = 0; y[n - 2u] = 0;
  x[n - 1u] = 0; y[n - 1u] = 0;
  for (size_t i = n - 4u; i < n; ++i) on[i] = 1;

  input.point_count = n;
  input.x = x;
  input.y = y;
  input.on_curve = on;
  input.contour_count = units->contour_count;
  input.ends = ends;
  r = gfnt_hint_run_glyph(ctx, &input, program, program_length, &run);
  if (r == GFNT_ERR_OOM) {
    goto done;
  }
  if (r != GFNT_OK) {
    *status = GFNT_HINT_STATUS_UNHINTED_PROGRAM_FAILED;
    if (fault && run) *fault = gfnt_hint_run_fault(run);
    r = gfnt_hint_unhinted(ctx, units, out, error);
    goto done;
  }
  *status = GFNT_HINT_STATUS_HINTED;
  if (normalised) {
    *normalised = gfnt_hint_run_normalised(run);
  }
  for (size_t i = 0; touches && i < touch_capacity && i < units->point_count; ++i) {
    touches[i] = gfnt_hint_run_touch(run, i);
  }
  if (advance_out) {
    int32_t p1x = 0, p2x = 0, dummy = 0;

    gfnt_hint_run_point(run, n - 4u, &p1x, &dummy);
    gfnt_hint_run_point(run, n - 3u, &p2x, &dummy);
    *advance_out = gfnt_hint_sub(p2x, p1x);
  }
  r = gfnt_outline_create(a, &hinted, error);
  if (r != GFNT_OK) {
    goto done;
  }
  {
    int32_t px = 0;
    int32_t py = 0;
    int32_t origin_x = 0;

    /* The outline is placed so that the origin phantom point is at x = 0. */
    gfnt_hint_run_point(run, n - 4u, &origin_x, &py);
    for (size_t c = 0; c < units->contour_count && r == GFNT_OK; ++c) {
      size_t first = c == 0 ? 0 : units->contours[c - 1];
      r = gfnt_outline_begin_contour(hinted, error);
      for (size_t i = first; r == GFNT_OK && i < units->contours[c]; ++i) {
        GFNT_Point p;
        gfnt_hint_run_point(run, i, &px, &py);
        p.x = px - origin_x;
        p.y = py;
        r = gfnt_outline_add_point(hinted, p, (GFNT_PointTag)units->tags[i], error);
      }
    }
  }
  if (r == GFNT_OK) {
    hinted->space = GFNT_OUTLINE_PIXELS;
    *out = hinted;
    hinted = NULL;
  }

done:
  gfnt_outline_destroy(hinted);
  gfnt_hint_run_destroy(run);
  gfnt_outline_destroy(units);
  if (x) a->free_fn(a->ctx, x);
  if (y) a->free_fn(a->ctx, y);
  if (on) a->free_fn(a->ctx, on);
  if (ends) a->free_fn(a->ctx, ends);
  return r;
}
