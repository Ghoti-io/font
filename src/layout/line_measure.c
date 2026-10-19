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
 * Measuring a line: its metrics, inline boxes and the strings shaped for hyphens and ellipses.
 */

#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/unicode/bidi.h>
#include <ghoti.io/unicode/break.h>
#include <ghoti.io/unicode/case.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/norm.h>
#include <ghoti.io/unicode/script.h>
#include <ghoti.io/unicode/utf.h>
#include <stdlib.h>
#include <string.h>
#include "kashida.h"
#include "layout_internal.h"

/** Shape a short string in a span's style and first face. */
GFNT_Result gfnt_lay_shape_string(const GFNT_Paragraph * p, size_t span,
    const uint32_t * text, size_t text_count,
    PGlyph * glyphs, size_t * count, GFNT_F26Dot6 * total, PRun * run, int force,
    GFNT_Error * error) {
  return gfnt_lay_shape_string_dir(p, span, text, text_count, glyphs, count, total, run, force, -1,
      ELL_GLYPHS, error);
}

/** As gfnt_lay_shape_string(), in a given direction: @p rtl 1 or 0, or -1 for the paragraph's own. */
GFNT_Result gfnt_lay_shape_string_dir(const GFNT_Paragraph * p, size_t span,
    const uint32_t * text, size_t text_count,
    PGlyph * glyphs, size_t * count, GFNT_F26Dot6 * total, PRun * run, int force, int rtl,
    size_t cap, GFNT_Error * error) {
  const GFNT_TextStyle * style = &p->styles[span];
  GFNT_ShapeOptions so;
  GFNT_FaceRuns fr;
  GFNT_Result r;
  uint16_t upem = 0;
  GFNT_F16Dot16 scale;
  bool upright;

  memset(&so, 0, sizeof so);
  memset(&fr, 0, sizeof fr);
  so.language = style->language;
  so.variation = style->variation;
  upright = text_count && (force >= 0 ? force == GFNT_ORIENT_RUN_UPRIGHT
      : gfnt_lay_string_orientation(p, span, text[0]) == GFNT_ORIENT_RUN_UPRIGHT);
  so.direction = upright ? GFNT_DIRECTION_TTB
                         : (rtl >= 0 ? rtl : (p->para_level & 1)) ? GFNT_DIRECTION_RTL
                                                                  : GFNT_DIRECTION_LTR;
  *count = 0;
  *total = 0;
  r = gfnt_faces_shape(style->faces, 1, text, text_count, &so, p->allocator,
      &fr, error);
  if (r != GFNT_OK) {
    return r;
  }
  memset(run, 0, sizeof *run);
  run->face = style->faces[0];
  run->span = span;
  run->level = p->para_level;
  run->orient = !text_count ? GFNT_ORIENT_RUN_HORIZONTAL
      : force >= 0 ? (uint8_t)force : gfnt_lay_string_orientation(p, span, text[0]);
  gfnt_lay_scale_metrics(run->face, style, run);
  if (!gfnt_lay_set_inline_metrics(run, style)) {
    gfnt_face_runs_free(&fr);
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a span has invalid computed line metrics");
  }
  if (gfnt_face_units_per_em(run->face, &upem, NULL) != GFNT_OK || !upem) {
    upem = 1000;
  }
  int64_t scale_wide = ((int64_t)style->size * 1024 + upem / 2) / upem;
  if (scale_wide > INT32_MAX) {
    gfnt_face_runs_free(&fr);
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "span size exceeds the supported metric range");
  }
  scale = (GFNT_F16Dot16)scale_wide;
  for (size_t k = 0; k < fr.count; k++) {
    for (size_t g = 0; g < fr.runs[k].run.count && *count < cap; g++) {
      const GFNT_ShapedGlyph * sg = &fr.runs[k].run.glyphs[g];
      PGlyph * pg = &glyphs[(*count)++];

      memset(pg, 0, sizeof *pg);
      pg->glyph = sg->glyph;
      pg->cluster = (uint32_t)sg->cluster;
      pg->has_ink = false;
      pg->advance = gfnt_units_to_pixels(upright ? -sg->y_advance : sg->x_advance, scale);
      pg->dx = gfnt_units_to_pixels(sg->x_offset, scale);
      pg->dy = gfnt_units_to_pixels(sg->y_offset, scale);
      *total += pg->advance;
      if (r == GFNT_OK) {
        r = gfnt_lay_glyph_ink(pg, run->face, style, scale, p->allocator);
      }
    }
  }
  gfnt_face_runs_free(&fr);
  return r;
}

/** An ink box in line coordinates, or nothing. */
typedef struct {
  bool set;
  GFNT_F26Dot6 l, t, r, b;
} InkBox;

bool gfnt_lay_inline_shift(const GFNT_Paragraph * p, const PRun * run,
    GFNT_F26Dot6 line_ascent, GFNT_F26Dot6 line_descent, GFNT_F26Dot6 * out) {
  const PRun * root = &p->fallback_metrics;
  int64_t shift;
  switch (run->vertical_align) {
    case GFNT_VERTICAL_ALIGN_TOP: shift = (int64_t)line_ascent - run->inline_ascent; break;
    case GFNT_VERTICAL_ALIGN_BOTTOM: shift = (int64_t)run->inline_descent - line_descent; break;
    case GFNT_VERTICAL_ALIGN_TEXT_TOP: shift = (int64_t)root->inline_ascent - run->inline_ascent; break;
    case GFNT_VERTICAL_ALIGN_TEXT_BOTTOM: shift = (int64_t)run->inline_descent - root->inline_descent; break;
    case GFNT_VERTICAL_ALIGN_MIDDLE:
      shift = ((int64_t)root->x_height - run->inline_ascent + run->inline_descent) / 2;
      break;
    default: shift = run->shift; break;
  }
  if (shift < INT32_MIN || shift > INT32_MAX) return false;
  *out = (GFNT_F26Dot6)shift;
  return true;
}

/** Measure the shaped units that will be placed on one line. */
LayoutLineState gfnt_lay_measure_line(const GFNT_Paragraph * p, size_t start, size_t end,
    GFNT_F26Dot6 width, GFNT_F26Dot6 top_y, GFNT_F26Dot6 tab_origin, GFNT_F26Dot6 * ew,
    GFNT_F26Dot6 * tabadv, GFNT_F26Dot6 * ruby_reclaim, GFNT_F26Dot6 * spacing_trim) {
  LayoutLineState state = {0};
  GFNT_F26Dot6 pos = 0;
  int64_t annotation_over = 0, annotation_under = 0;
  bool inline_mode = p->options.strut;
  bool inline_has_box = false, annotation_on_line = false;

  for (size_t j = start; j < end; j++) {
    const PRun * run = &p->runs[p->units[j].run];
    const GFNT_TextStyle * style = &p->styles[run->span];
    if (style->line_height.kind != GFNT_LINE_HEIGHT_NORMAL
        || run->vertical_align != GFNT_VERTICAL_ALIGN_BASELINE) inline_mode = true;
  }
  for (size_t i = 0; i < p->annotation_count; i++) {
    if (p->annotations[i].unit_end > start && p->annotations[i].unit_start < end) {
      inline_mode = true;
      annotation_on_line = true;
    }
  }
  if (inline_mode) {
    state.ascent = state.descent = INT32_MIN;
    state.gap = -1; /* sentinel: placement uses the union box */
  }

  state.unit_start = start;
  state.unit_end = end;
  state.available_width = width;
  state.top_y = top_y;
  for (size_t j = start; j < end; j++) spacing_trim[j] = 0;
  for (size_t j = start; j < end; j++) {
    const Unit * unit = &p->units[j];
    const PRun * run = &p->runs[unit->run];
    int64_t trim_current = 0, trim_previous = 0;
    size_t group_start = 0;
    const PAnnotation * group = NULL;
    GFNT_F26Dot6 refund = gfnt_lay_ruby_right_refund(p, start, j, &group, &group_start);

    if (unit->g_hi > unit->g_lo) ruby_reclaim[unit->g_hi - 1] = 0;
    if (refund) {
      if (!group) {
        state.invalid = true;
      }
      else {
        GFNT_F26Dot6 left = refund;
        for (size_t k = j; k > group_start && left > 0;) {
          Unit * base = &p->units[--k];
          GFNT_F26Dot6 share = gfnt_lay_ruby_unit_share(group, k, group->break_extra);
          if (share > left) share = left;
          if (share > ew[k]) {
            state.invalid = true;
            break;
          }
          ew[k] -= share;
          pos -= share;
          left -= share;
          if (base->g_hi > base->g_lo) ruby_reclaim[base->g_hi - 1] += share;
        }
        if (left) state.invalid = true;
      }
    }

    if (j > start && p->units[j - 1].autospace_after) {
      ew[j - 1] += p->units[j - 1].autospace_after;
      pos += p->units[j - 1].autospace_after;
    }
    if (j == start && gfnt_lay_text_spacing_open_at_start(p, start)) {
      trim_current = gfnt_lay_text_spacing_half_ic(p, j);
    }
    else if (j > start) {
      if (gfnt_lay_text_spacing_trim_open_pair(p, j - 1, j)) {
        trim_current = gfnt_lay_text_spacing_half_ic(p, j);
      }
      if (gfnt_lay_text_spacing_trim_close_pair(p, j - 1, j)) {
        trim_previous = gfnt_lay_text_spacing_half_ic(p, j - 1);
      }
    }
    if (trim_previous) {
      if (trim_previous > ew[j - 1]) state.invalid = true;
      else {
        ew[j - 1] -= (GFNT_F26Dot6)trim_previous;
        pos -= trim_previous;
        spacing_trim[j - 1] += (GFNT_F26Dot6)trim_previous;
      }
    }
    ew[j] = gfnt_lay_eff_width_at(p, unit, pos, tab_origin);
    if (trim_current > ew[j]) trim_current = ew[j];
    ew[j] -= (GFNT_F26Dot6)trim_current;
    spacing_trim[j] += (GFNT_F26Dot6)trim_current;
    pos += ew[j];
    if (unit->tab) {
      tabadv[unit->g_lo] = ew[j];
    }
    if (inline_mode) {
      int64_t a = run->inline_ascent, d = run->inline_descent;
      GFNT_F26Dot6 shift = 0;
      if (run->vertical_align == GFNT_VERTICAL_ALIGN_TOP
          || run->vertical_align == GFNT_VERTICAL_ALIGN_BOTTOM) continue;
      if (!gfnt_lay_inline_shift(p, run, 0, 0, &shift)) {
        state.invalid = true;
        continue;
      }
      a += shift; d -= shift;
      if (a < INT32_MIN || a > INT32_MAX || d < INT32_MIN || d > INT32_MAX) {
        state.invalid = true;
        continue;
      }
      inline_has_box = true;
      if (a > state.ascent) state.ascent = a;
      if (d > state.descent) state.descent = d;
    } else {
      int64_t thickness = (int64_t)run->ascent + run->descent + run->gap;
      if (thickness < INT32_MIN || thickness > INT32_MAX) {
        state.invalid = true;
        continue;
      }
      if (run->ascent > state.ascent) state.ascent = run->ascent;
      if (run->descent > state.descent) state.descent = run->descent;
      if (run->gap > state.gap) state.gap = run->gap;
      if (thickness > state.thickness) {
        state.thickness = (GFNT_F26Dot6)thickness;
      }
    }
  }
  if (inline_mode && p->options.strut) {
    const PRun * root = &p->fallback_metrics;
    int64_t a = (int64_t)root->inline_ascent + root->shift;
    int64_t d = (int64_t)root->inline_descent - root->shift;
    if (a < INT32_MIN || a > INT32_MAX || d < INT32_MIN || d > INT32_MAX) {
      state.invalid = true;
    }
    inline_has_box = true;
    if (a > state.ascent) state.ascent = a;
    if (d > state.descent) state.descent = d;
  }
  for (size_t i = 0; i < p->annotation_count; i++) {
    const PAnnotation * annotation = &p->annotations[i];
    int64_t height;
    if (annotation->unit_end <= start || annotation->unit_start >= end) continue;
    height = (int64_t)annotation->ascent + annotation->descent;
    if (annotation->descriptor.position == GFNT_ANNOTATION_UNDER) annotation_under += height;
    else annotation_over += height;
  }
  if (annotation_over > INT32_MAX || annotation_under > INT32_MAX) state.invalid = true;
  if (annotation_over > 0) {
    int64_t a = (int64_t)state.ascent + annotation_over;
    if (a > INT32_MAX) state.invalid = true;
    else state.ascent = (GFNT_F26Dot6)a;
  }
  if (annotation_under > 0) {
    int64_t d = (int64_t)state.descent + annotation_under;
    if (d > INT32_MAX) state.invalid = true;
    else state.descent = (GFNT_F26Dot6)d;
  }
  if (annotation_on_line) inline_has_box = true;
  if (inline_mode && !inline_has_box && start < end) {
    const PRun * run = &p->runs[p->units[start].run];
    state.ascent = run->inline_ascent;
    state.descent = run->inline_descent;
  }
  if (inline_mode && start < end) {
    for (size_t j = start; j < end; j++) {
      const PRun * run = &p->runs[p->units[j].run];
      int64_t a, d;
      GFNT_F26Dot6 shift;
      if (run->vertical_align != GFNT_VERTICAL_ALIGN_TOP
          && run->vertical_align != GFNT_VERTICAL_ALIGN_BOTTOM) continue;
      if (!gfnt_lay_inline_shift(p, run, state.ascent, state.descent, &shift)) {
        state.invalid = true;
        continue;
      }
      a = (int64_t)run->inline_ascent + shift;
      d = (int64_t)run->inline_descent - shift;
      if (a < INT32_MIN || a > INT32_MAX || d < INT32_MIN || d > INT32_MAX) {
        state.invalid = true;
        continue;
      }
      if (a > state.ascent) state.ascent = (GFNT_F26Dot6)a;
      if (d > state.descent) state.descent = (GFNT_F26Dot6)d;
    }
  }
  state.hang_from = end;
  if (state.hang_from > start && p->units[state.hang_from - 1].hard) {
    state.hang_from--;
  }
  while (p->options.white_space != GFNT_WHITE_SPACE_PRE
      && p->options.white_space != GFNT_WHITE_SPACE_BREAK_SPACES
      && state.hang_from > start && p->units[state.hang_from - 1].space) {
    state.hang_from--;
  }
  for (size_t j = start; j < state.hang_from; j++) {
    state.natural += ew[j];
  }
  if (state.natural > width && state.hang_from > start
      && p->options.text_spacing_trim != GFNT_TEXT_SPACING_TRIM_SPACE_ALL) {
    size_t last = state.hang_from - 1;
    if (gfnt_lay_text_spacing_punctuation(p, &p->units[last]) == TEXT_SPACING_CLOSE) {
      int64_t trim_end = gfnt_lay_text_spacing_half_ic(p, last);
      if (trim_end && state.natural - trim_end <= width && trim_end <= ew[last]) {
        ew[last] -= (GFNT_F26Dot6)trim_end;
        state.natural -= (GFNT_F26Dot6)trim_end;
        spacing_trim[last] += (GFNT_F26Dot6)trim_end;
      }
    }
  }
  if (start == end && !p->options.strut) {
    const PRun * run = start < p->unit_count ? &p->runs[p->units[start].run]
                                              : &p->fallback_metrics;
    const PRun * source = p->run_count ? (start > 0 ? &p->runs[p->units[start - 1].run] : run)
                                        : run;

    state.ascent = source->ascent;
    state.descent = source->descent;
    state.gap = source->gap;
    state.thickness = state.ascent + state.descent + state.gap;
  }
  if (inline_mode) {
    int64_t thickness = (int64_t)state.ascent + state.descent;
    if (thickness < INT32_MIN || thickness > INT32_MAX) state.invalid = true;
    else state.thickness = (GFNT_F26Dot6)thickness;
  }
  return state;
}
