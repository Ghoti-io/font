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
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/shape.h>
#include <ghoti.io/unicode/bidi.h>
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

/** What a cluster has drawn so far, in ems: where the next mark stacks. */
typedef struct Stack {
  bool valid;
  double top;      ///< The highest ink, above the baseline.
  double bottom;   ///< The lowest ink.
  double advance;  ///< How wide the base is.
} Stack;

static double em_of(const GFNT_Face * face) {
  uint16_t upem = 0;
  return gfnt_face_units_per_em(face, &upem, NULL) == GFNT_OK && upem
      ? (double)upem : 1000.0;
}

/** The ink of a glyph in ems, y up; false when it has none. */
static bool ink(const GFNT_Face * face, const GFNT_ShapedGlyph * g,
    const GFNT_ShapeOptions * options, double * top, double * bottom,
    double * centre) {
  GFNT_Extents e;
  double em = em_of(face);
  if (!gfnt_glyph_extents(face, g->glyph, options->variation, &e)
      || (e.width == 0 && e.height == 0)) {
    return false;
  }
  *top = (g->y_offset + e.y_bearing) / em;
  *bottom = (g->y_offset + e.y_bearing + e.height) / em;
  *centre = (g->x_offset + e.x_bearing + e.width / 2.0) / em;
  return true;
}

/**
 * Take the state of a stretch's last cluster, as the stretch drew it: the ink of
 * the base and of the marks the font put with it.
 */
static void measure(const GFNT_FaceRun * run, const GFNT_Face * face,
    const uint32_t * cp, const GFNT_ShapeOptions * options, Stack * st) {
  size_t base = run->start + run->length;
  double em = em_of(face);
  while (base > run->start && is_attached(cp[base - 1])) {
    base--;
  }
  if (base == run->start) {
    return; // all marks: the state holds from before
  }
  base--;
  st->valid = true;
  st->top = st->bottom = st->advance = 0;
  for (size_t g = 0; g < run->run.count; g++) {
    const GFNT_ShapedGlyph * m = &run->run.glyphs[g];
    double t, b, c;
    if (m->cluster < base) {
      continue;
    }
    st->advance += m->x_advance / em;
    if (ink(face, m, options, &t, &b, &c)) {
      st->top = t > st->top ? t : st->top;
      st->bottom = b < st->bottom ? b : st->bottom;
    }
  }
}

/**
 * Put the leading marks of a stretch, which a change of face cut from their
 * base, over the base: centred, with no advance, and stacked above or below
 * whatever the cluster has drawn so far.
 */
static void stack_over(GFNT_FaceRun * run, const GFNT_Face * face,
    const uint32_t * cp, const GFNT_ShapeOptions * options, bool rtl, Stack * st) {
  size_t lead = run->start;
  double em = em_of(face);
  const double gap = 0.02;
  while (lead < run->start + run->length && is_mark(cp[lead])) {
    lead++;
  }
  if (!st->valid) {
    return;
  }
  // In the order of the text, whichever way the glyphs lie.
  for (size_t k = 0; k < run->run.count; k++) {
    GFNT_ShapedGlyph * m = &run->run.glyphs[rtl ? run->run.count - 1 - k : k];
    double t, b, c;
    double x;
    if (m->cluster >= lead) {
      continue;
    }
    if (!ink(face, m, options, &t, &b, &c)) {
      c = m->x_offset / em; // no ink: centred on its origin
      t = b = 0;
    }
    // The ink as the shaper left it; put its centre over the base's.
    x = (rtl ? st->advance / 2 : -st->advance / 2) - (c - m->x_offset / em);
    m->x_offset = (int32_t)(x * em + (x < 0 ? -0.5 : 0.5));
    m->x_advance = 0;
    if (t != b) {
      // Its extent, with the vertical offset the shaper gave it taken out.
      double h_top = t - m->y_offset / em;
      double h_bottom = b - m->y_offset / em;
      double y;
      // Which side it hangs on is where its own ink lies: the shaper has put
      // marks in canonical order and merged their clusters, so the character
      // is no longer to hand.
      if (h_top + h_bottom < 0) {
        y = st->bottom - h_top - gap;
        st->bottom = y + h_bottom;
      } else {
        y = st->top - h_bottom + gap;
        st->top = y + h_top;
      }
      m->y_offset = (int32_t)(y * em + (y < 0 ? -0.5 : 0.5));
    }
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
  Stack stack = {false, 0, 0, 0};

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
      if (!vertical && is_mark(codepoints[i])) {
        stack_over(run, faces[pick[i]], codepoints, &resolved, rtl, &stack);
      }
      measure(run, faces[pick[i]], codepoints, &resolved, &stack);
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

/** Shape one level run into @p out, rebased to the whole text. */
static GFNT_Result bidi_part(const GFNT_Face * const * faces, size_t face_count,
    const uint32_t * codepoints, size_t start, size_t length, bool rtl,
    const GFNT_ShapeOptions * options, const GFNT_Allocator * allocator,
    GFNT_FaceRuns * out, GFNT_Error * error) {
  GFNT_ShapeOptions part = *options;
  GFNT_ShapeFeature * features = NULL;
  GFNT_Result r;

  part.direction = rtl ? GFNT_DIRECTION_RTL : GFNT_DIRECTION_LTR;
  r = cut_features(options, start, length, allocator, &features,
      &part.feature_count);
  if (r != GFNT_OK) {
    return gfnt_error_set(error, r, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the features of a level run");
  }
  part.features = features;
  r = gfnt_faces_shape(faces, face_count, codepoints + start, length, &part,
      allocator, out, error);
  if (features) {
    allocator->free_fn(allocator->ctx, features);
  }
  for (size_t i = 0; r == GFNT_OK && i < out->count; i++) {
    out->runs[i].start += start;
    for (size_t g = 0; g < out->runs[i].run.count; g++) {
      out->runs[i].run.glyphs[g].cluster += (uint32_t)start;
    }
  }
  return r;
}

GFNT_Result gfnt_faces_shape_bidi(const GFNT_Face * const * faces,
    size_t face_count, const uint32_t * codepoints, size_t count,
    GFNT_BidiDirection paragraph, const GFNT_ShapeOptions * options,
    const GFNT_Allocator * allocator, GFNT_FaceRuns * out, GFNT_Error * error) {
  static const GFNT_ShapeOptions defaults;
  uint8_t * levels = NULL;
  size_t * run_start = NULL;
  uint8_t * run_level = NULL;
  size_t * order = NULL;
  GFNT_FaceRuns * parts = NULL;
  size_t nruns = 0;
  size_t total = 0;
  GFNT_FaceRuns result;
  GFNT_Result r = GFNT_OK;
  GUNI_BidiDirection dir;

  gfnt_error_clear(error);
  if (!faces || !face_count || !out || (count && !codepoints)
      || (unsigned)paragraph > GFNT_BIDI_AUTO) {
    return GFNT_ERR_INVALID;
  }
  if (!options) {
    options = &defaults;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  // Vertical text has no bidi levels of its own: it is shaped as one run.
  if (options->direction == GFNT_DIRECTION_TTB
      || options->direction == GFNT_DIRECTION_BTT || count == 0) {
    return gfnt_faces_shape(faces, face_count, codepoints, count, options,
        allocator, out, error);
  }
  dir = paragraph == GFNT_BIDI_RTL ? GUNI_BIDI_RTL
      : paragraph == GFNT_BIDI_AUTO ? GUNI_BIDI_AUTO : GUNI_BIDI_LTR;
  levels = allocator->calloc_fn(allocator->ctx, count, sizeof *levels);
  run_start = allocator->calloc_fn(allocator->ctx, count + 1, sizeof *run_start);
  run_level = allocator->calloc_fn(allocator->ctx, count, sizeof *run_level);
  order = allocator->calloc_fn(allocator->ctx, count, sizeof *order);
  parts = allocator->calloc_fn(allocator->ctx, count, sizeof *parts);
  memset(&result, 0, sizeof result);
  result.allocator = allocator;
  if (!levels || !run_start || !run_level || !order || !parts) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the bidirectional levels");
    goto done;
  }
  if (guni_bidi_levels_with_allocator(codepoints, count, dir, NULL, levels,
          count, NULL, NULL) != GUNI_OK) {
    r = gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "the text is too long for the bidirectional algorithm");
    goto done;
  }
  for (size_t i = 0; i < count; i++) {
    if (i == 0 || levels[i] != levels[i - 1]) {
      run_start[nruns] = i;
      run_level[nruns++] = levels[i];
    }
  }
  run_start[nruns] = count;
  for (size_t k = 0; k < nruns && r == GFNT_OK; k++) {
    r = bidi_part(faces, face_count, codepoints, run_start[k],
        run_start[k + 1] - run_start[k], (run_level[k] & 1) != 0, options,
        allocator, &parts[k], error);
    total += parts[k].count;
  }
  if (r == GFNT_OK
      && guni_bidi_reorder(run_level, nruns, order, count) != GUNI_OK) {
    r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "the level runs could not be put in visual order");
  }
  if (r == GFNT_OK) {
    result.runs = allocator->calloc_fn(allocator->ctx, total ? total : 1,
        sizeof *result.runs);
    if (!result.runs) {
      r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory for the stretches");
    }
  }
  if (r == GFNT_OK) {
    // The stretches of each level run are already in visual order within it;
    // the level runs are taken in the order the reordering gives.
    for (size_t v = 0; v < nruns; v++) {
      GFNT_FaceRuns * p = &parts[order[v]];

      for (size_t i = 0; i < p->count; i++) {
        result.runs[result.count++] = p->runs[i];
      }
      if (p->runs) {
        allocator->free_fn(allocator->ctx, p->runs);
        p->runs = NULL;
        p->count = 0;
      }
    }
    *out = result;
  }
done:
  if (parts) {
    for (size_t k = 0; k < count; k++) {
      gfnt_face_runs_free(&parts[k]);
    }
  }
  allocator->free_fn(allocator->ctx, parts);
  allocator->free_fn(allocator->ctx, order);
  allocator->free_fn(allocator->ctx, run_level);
  allocator->free_fn(allocator->ctx, run_start);
  allocator->free_fn(allocator->ctx, levels);
  return r;
}
