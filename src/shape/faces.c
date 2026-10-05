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
 * Font fallback: a text shaped with the first face of a list that has each of its
 * characters. See `shape.h`.
 */

#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/shape.h>
#include <ghoti.io/unicode/char.h>
#include <stdlib.h>
#include <string.h>
#include "../layout_tables/layout.h"
#include "fallback.h"
#include "uprops.h"

/** Whether the face maps the code point to a glyph other than `.notdef`. */
static bool covers(const GFNT_Face * face, uint32_t u) {
  uint32_t glyph = 0;
  return gfnt_face_glyph_for_codepoint(face, u, &glyph, NULL) == GFNT_OK
      && glyph != 0;
}

static bool is_mark(uint32_t u) {
  return (guni_general_category_mask(u) & GUNI_GC_MASK_M) != 0;
}

/** A character that stays with the one before it whatever font draws it. */
static bool is_attached(uint32_t u) {
  return is_mark(u) || gfnt_u_default_ignorable(u);
}

static size_t first_covering(const GFNT_Face * const * faces, size_t n,
    uint32_t u, size_t otherwise) {
  for (size_t f = 0; f < n; f++) {
    if (covers(faces[f], u)) {
      return f;
    }
  }
  return otherwise;
}

/** Choose a face for each code point: one per cluster where one has it all. */
static void choose(const GFNT_Face * const * faces, size_t n,
    const uint32_t * cp, size_t count, size_t * pick) {
  size_t i = 0;
  size_t previous = 0;
  while (i < count) {
    size_t end = i + 1;
    size_t whole = n;
    size_t base;
    while (end < count && is_attached(cp[end])) {
      end++;
    }
    if (gfnt_u_default_ignorable(cp[i]) && !is_mark(cp[i])) {
      // Nothing to draw: it goes where the text before it went.
      pick[i] = previous;
    } else {
      for (size_t f = 0; f < n && whole == n; f++) {
        size_t k;
        for (k = i; k < end; k++) {
          if (!gfnt_u_default_ignorable(cp[k]) && !covers(faces[f], cp[k])) {
            break;
          }
        }
        if (k == end) {
          whole = f;
        }
      }
      base = whole < n ? whole : first_covering(faces, n, cp[i], 0);
      pick[i] = base;
      previous = base;
    }
    for (size_t k = i + 1; k < end; k++) {
      if (whole < n || gfnt_u_default_ignorable(cp[k])
          || covers(faces[pick[i]], cp[k])) {
        pick[k] = pick[i];
      } else {
        pick[k] = first_covering(faces, n, cp[k], pick[i]);
      }
    }
    i = end;
  }
}

/** The features of the run, cut to [start, start + length) and rebased. */
static GFNT_Result cut_features(const GFNT_ShapeOptions * options, size_t start,
    size_t length, const GFNT_Allocator * allocator, GFNT_ShapeFeature ** out,
    size_t * out_count) {
  GFNT_ShapeFeature * list = NULL;
  size_t m = 0;
  *out = NULL;
  *out_count = 0;
  if (!options->feature_count) {
    return GFNT_OK;
  }
  list = allocator->calloc_fn(allocator->ctx, options->feature_count,
      sizeof *list);
  if (!list) {
    return GFNT_ERR_OOM;
  }
  for (size_t k = 0; k < options->feature_count; k++) {
    GFNT_ShapeFeature f = options->features[k];
    size_t end = f.end == GFNT_SHAPE_END ? GFNT_SHAPE_END : f.end;
    if (f.start >= start + length || (end != GFNT_SHAPE_END && end <= start)) {
      continue;
    }
    f.start = f.start > start ? f.start - start : 0;
    f.end = (end == GFNT_SHAPE_END || end >= start + length) ? GFNT_SHAPE_END
        : end - start;
    // A range that ran to the end of the text still runs to the end of this part
    // only if it ran past it.
    list[m++] = f;
  }
  *out = list;
  *out_count = m;
  return GFNT_OK;
}

/**
 * Give the leading marks of a stretch, which were cut from their base by a change
 * of face, no advance and a place over the base's glyph.
 */
static void centre_over(const GFNT_FaceRun * before, GFNT_FaceRun * run,
    const GFNT_Face * before_face, const GFNT_Face * face,
    const uint32_t * cp, const GFNT_ShapeOptions * options, bool rtl) {
  const GFNT_ShapedGlyph * base;
  size_t lead = run->start;
  GFNT_Extents e;
  int32_t cx;
  if (!before->run.count) {
    return;
  }
  while (lead < run->start + run->length && is_mark(cp[lead])) {
    lead++;
  }
  // The base is the last glyph, in text order, of the stretch before.
  base = rtl ? &before->run.glyphs[0] : &before->run.glyphs[before->run.count - 1];
  (void)before_face;
  for (size_t g = 0; g < run->run.count; g++) {
    GFNT_ShapedGlyph * m = &run->run.glyphs[g];
    if (m->cluster >= lead) {
      continue;
    }
    cx = gfnt_glyph_extents(face, m->glyph, options->variation, &e)
        ? e.x_bearing + e.width / 2 : m->x_advance / 2;
    // Where the ink is now is the offset plus the ink's own centre; put it over
    // the middle of the base instead.
    m->x_offset = (rtl ? base->x_advance / 2 : -(base->x_advance / 2)) - cx;
    m->x_advance = 0;
  }
}

GFNT_Result gfnt_faces_shape(const GFNT_Face * const * faces, size_t face_count,
    const uint32_t * codepoints, size_t count, const GFNT_ShapeOptions * options,
    const GFNT_Allocator * allocator, GFNT_FaceRuns * out, GFNT_Error * error) {
  static const GFNT_ShapeOptions defaults;
  GFNT_ShapeOptions resolved;
  GFNT_FaceRuns result;
  size_t * pick = NULL;
  size_t segments = 0;
  GFNT_Result r = GFNT_OK;
  bool rtl;
  bool vertical;

  gfnt_error_clear(error);
  if (!faces || !face_count || !out || (count && !codepoints)) {
    return GFNT_ERR_INVALID;
  }
  for (size_t f = 0; f < face_count; f++) {
    if (!faces[f]) {
      return GFNT_ERR_INVALID;
    }
  }
  if (!options) {
    options = &defaults;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  memset(&result, 0, sizeof result);
  result.allocator = allocator;
  if (count == 0) {
    *out = result;
    return GFNT_OK;
  }
  resolved = *options;
  if (!resolved.script) {
    resolved.script = gfnt_shape_script_of(codepoints, count);
  }
  rtl = options->direction == GFNT_DIRECTION_RTL
      || options->direction == GFNT_DIRECTION_BTT;
  vertical = options->direction == GFNT_DIRECTION_TTB
      || options->direction == GFNT_DIRECTION_BTT;

  pick = allocator->calloc_fn(allocator->ctx, count, sizeof *pick);
  if (!pick) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the choice of faces");
  }
  choose(faces, face_count, codepoints, count, pick);
  for (size_t i = 0; i < count; i++) {
    if (i == 0 || pick[i] != pick[i - 1]) {
      segments++;
    }
  }
  result.runs = allocator->calloc_fn(allocator->ctx, segments,
      sizeof *result.runs);
  if (!result.runs) {
    allocator->free_fn(allocator->ctx, pick);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the stretches");
  }
  // Logical order first; reversed below for a run that reads backwards.
  for (size_t i = 0; i < count && r == GFNT_OK;) {
    size_t end = i + 1;
    GFNT_ShapeOptions part = resolved;
    GFNT_ShapeFeature * features = NULL;
    GFNT_FaceRun * run = &result.runs[result.count];
    while (end < count && pick[end] == pick[i]) {
      end++;
    }
    r = cut_features(&resolved, i, end - i, allocator, &features,
        &part.feature_count);
    if (r != GFNT_OK) {
      gfnt_error_set(error, r, 0, 0, GFNT_GLYPH_NONE,
          "no memory for the features of a stretch");
      break;
    }
    part.features = features;
    run->face = pick[i];
    run->start = i;
    run->length = end - i;
    r = gfnt_face_shape(faces[pick[i]], codepoints + i, end - i, &part,
        allocator, &run->run, error);
    if (features) {
      allocator->free_fn(allocator->ctx, features);
    }
    if (r == GFNT_OK) {
      for (size_t g = 0; g < run->run.count; g++) {
        run->run.glyphs[g].cluster += (uint32_t)i;
      }
      result.count++;
      if (!vertical && result.count > 1 && is_mark(codepoints[i])) {
        centre_over(&result.runs[result.count - 2], run,
            faces[result.runs[result.count - 2].face], faces[pick[i]],
            codepoints, &resolved, rtl);
      }
    }
    i = end;
  }
  allocator->free_fn(allocator->ctx, pick);
  if (r != GFNT_OK) {
    gfnt_face_runs_free(&result);
    return r;
  }
  if (rtl) {
    for (size_t a = 0, b = result.count - 1; a < b; a++, b--) {
      GFNT_FaceRun t = result.runs[a];
      result.runs[a] = result.runs[b];
      result.runs[b] = t;
    }
  }
  *out = result;
  return GFNT_OK;
}

void gfnt_face_runs_free(GFNT_FaceRuns * runs) {
  if (!runs) {
    return;
  }
  for (size_t i = 0; i < runs->count; i++) {
    gfnt_shaped_run_free(&runs->runs[i].run);
  }
  if (runs->runs && runs->allocator) {
    runs->allocator->free_fn(runs->allocator->ctx, runs->runs);
  }
  memset(runs, 0, sizeof *runs);
}
