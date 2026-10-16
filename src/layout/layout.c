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
 * Laying a paragraph out: the layout state, the line loop and the public layout calls.
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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "kashida.h"
#include "layout_internal.h"

static GFNT_Result paragraph_layout_internal(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    const GFNT_LineConstraint * constraint, GFNT_Layout * out, GFNT_Error * error);

void gfnt_layout_free(GFNT_Layout * layout) {
  if (!layout || !layout->allocator) {
    return;
  }
  for (size_t li = 0; li < layout->line_count; li++) {
    GFNT_Line * line = &layout->lines[li];
    gfnt_lay_free_line_annotation_tree(layout->allocator,
        (GFNT_LineAnnotation *)line->annotations, line->annotation_count);
  }
  layout->allocator->free_fn(layout->allocator->ctx, layout->lines);
  layout->allocator->free_fn(layout->allocator->ctx, layout->storage_);
  memset(layout, 0, sizeof *layout);
}

typedef struct {
  const GFNT_Paragraph * paragraph;
  Slice * slices;
  uint8_t * levels;
  size_t * order;
  GFNT_Line * line;
  size_t unit_start, unit_end, hang_from, slice_count;
  bool hyphenated, truncated, vertical;
  PGlyph * hyphen_glyphs, * ellipsis_glyphs;
  size_t hyphen_count, ellipsis_count;
  PRun * hyphen_run, * ellipsis_run;
  LayoutLineState * measured;
  GFNT_F26Dot6 top_y;
} LineBuildState;

/** Reserve every paragraph and output buffer before placement begins. */
static GFNT_Result layout_state_allocate(LayoutState * state, const GFNT_Paragraph * p,
    GFNT_F26Dot6 width, const GFNT_LineConstraint * constraint, GFNT_Error * error) {
  const GFNT_Allocator * allocator = state->allocator;
  GFNT_F26Dot6 max_content;

  if (gfnt_paragraph_max_content(p, &max_content) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "paragraph content width exceeds the 26.6 coordinate range");
  }

  state->starts = allocator->calloc_fn(allocator->ctx, p->unit_count + 2,
      sizeof *state->starts);
  state->slices = allocator->calloc_fn(allocator->ctx, p->unit_count + 1,
      sizeof *state->slices);
  state->order = allocator->calloc_fn(allocator->ctx, p->unit_count + 1,
      sizeof *state->order);
  state->slice_levels = allocator->calloc_fn(allocator->ctx, p->unit_count + 1, 1);
  state->ew = allocator->calloc_fn(allocator->ctx, p->unit_count + 1, sizeof *state->ew);
  state->tabadv = allocator->calloc_fn(allocator->ctx, p->glyph_count + 1,
      sizeof *state->tabadv);
  state->ruby_reclaim = allocator->calloc_fn(allocator->ctx, p->glyph_count + 1,
      sizeof *state->ruby_reclaim);
  state->spacing_trim = allocator->calloc_fn(allocator->ctx, p->unit_count + 1,
      sizeof *state->spacing_trim);
  state->gjf = allocator->calloc_fn(allocator->ctx, p->glyph_count + 1, 1);
  state->hyphened = allocator->calloc_fn(allocator->ctx, p->unit_count + 2,
      sizeof *state->hyphened);
  state->gk = allocator->calloc_fn(allocator->ctx, p->kcount ? p->glyph_count + 1 : 1,
      sizeof *state->gk);
  state->kres = allocator->calloc_fn(allocator->ctx, p->kcount + 1, sizeof *state->kres);
  state->cand_at = allocator->calloc_fn(allocator->ctx, p->unit_count + 1,
      sizeof *state->cand_at);
  state->cand_fit = allocator->calloc_fn(allocator->ctx, p->unit_count + 1,
      sizeof *state->cand_fit);
  state->band_starts = allocator->calloc_fn(allocator->ctx, p->unit_count + 2,
      sizeof *state->band_starts);
  state->band_widths = allocator->calloc_fn(allocator->ctx, p->unit_count + 2,
      sizeof *state->band_widths);
  state->line_tops = allocator->calloc_fn(allocator->ctx, p->unit_count + 2,
      sizeof *state->line_tops);
  if (!state->starts || !state->slices || !state->order || !state->slice_levels
      || !state->ew || !state->tabadv || !state->ruby_reclaim || !state->spacing_trim || !state->gjf
      || !state->hyphened || !state->gk
      || !state->kres || !state->cand_at || !state->cand_fit || !state->band_starts
      || !state->band_widths || !state->line_tops) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the layout");
  }
  if (!constraint) {
    state->line_count = gfnt_lay_break_lines_balanced(p, width, state->starts, state->hyphened,
        state->cand_at, state->cand_fit, state->band_starts, state->band_widths);
    GFNT_F26Dot6 top = 0;
    for (size_t li = 0; li < state->line_count; li++) {
      state->line_tops[li] = top;
      LayoutLineState measured = gfnt_lay_measure_line(p, state->starts[li], state->starts[li + 1],
          state->band_widths[li], top,
          p->options.tabs_from_container ? state->band_starts[li] : 0,
          state->ew, state->tabadv, state->ruby_reclaim, state->spacing_trim);
      if (measured.invalid || measured.thickness < 0
          || top > INT32_MAX - measured.thickness) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "line metrics exceed the supported layout range");
      }
      top += measured.thickness;
    }
  }
  else {
    size_t unit = 0;
    GFNT_F26Dot6 top = 0;
    size_t attempts = 0;
    const size_t callback_budget = p->unit_count > (SIZE_MAX - 16) / 2
        ? SIZE_MAX : 2 * p->unit_count + 16;
    bool trailing_empty = false;
    do {
      GFNT_Band raw_band = {0}, band = {0};
      size_t line_start = unit;
      BreakResult br;
      LayoutLineState measured;
      GFNT_F26Dot6 height;
      size_t retries = 0;
      bool stable = false;
      bool have_candidate = false;
      GFNT_Result callback_result;

      do {
        if (!have_candidate) {
          if (++attempts > callback_budget) {
            return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
                "line constraint did not stabilize within its callback budget");
          }
          callback_result = constraint->band(constraint->ctx, top, 0, &raw_band);
          if (callback_result != GFNT_OK) return callback_result;
          if (raw_band.width < 0 || raw_band.next_top < 0
              || (int64_t)raw_band.start + raw_band.width > INT32_MAX) {
            return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
                "line constraint returned an invalid band");
          }
          band = raw_band;
          if (!gfnt_lay_indent_band(p, state->line_count, &band)) {
            return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
                "line indentation exceeds the supported coordinate range");
          }
          have_candidate = true;
        }
        if (!band.width && band.next_top > top) {
          top = band.next_top;
          have_candidate = false;
          continue;
        }
        GFNT_F26Dot6 tab_origin = p->options.tabs_from_container ? band.start : 0;
        br = gfnt_lay_break_one_line(p, unit, band.width, tab_origin, state->cand_at, state->cand_fit);
        measured = gfnt_lay_measure_line(p, unit, br.end, band.width, top, tab_origin, state->ew,
            state->tabadv, state->ruby_reclaim, state->spacing_trim);
        if (measured.invalid || measured.thickness < 0) {
          return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
              "line metrics exceed the supported layout range");
        }
        height = measured.thickness;
        {
          GFNT_Band actual = {0};
          if (++attempts > callback_budget) {
            return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
                "line constraint did not stabilize within its callback budget");
          }
          callback_result = constraint->band(constraint->ctx, top, height, &actual);
          if (callback_result != GFNT_OK) return callback_result;
          if (actual.width < 0 || actual.next_top < 0
              || (int64_t)actual.start + actual.width > INT32_MAX) {
            return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
                "line constraint returned an invalid band");
          }
          if (actual.start == raw_band.start && actual.width == raw_band.width) {
            raw_band = actual;
            band = actual;
            if (!gfnt_lay_indent_band(p, state->line_count, &band)) {
              return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
                  "line indentation exceeds the supported coordinate range");
            }
            stable = true;
          }
          else {
            raw_band = actual;
            band = actual;
            if (!gfnt_lay_indent_band(p, state->line_count, &band)) {
              return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
                  "line indentation exceeds the supported coordinate range");
            }
            have_candidate = true;
            if (++retries > callback_budget) break;
          }
        }
      } while (!stable && retries <= callback_budget);
      if (!stable) {
        return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
            "line constraint changed continuously while measuring a line");
      }
      if (measured.natural > band.width && band.next_top > top) {
        top = band.next_top;
        have_candidate = false;
        continue;
      }
      state->starts[state->line_count] = line_start;
      state->hyphened[state->line_count] = br.hyphen;
      state->band_starts[state->line_count] = band.start;
      state->band_widths[state->line_count] = band.width;
      state->line_tops[state->line_count] = top;
      state->line_count++;
      if (br.end <= unit && unit < p->unit_count) {
        return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
            "line breaker did not advance");
      }
      unit = br.end;
      if (line_start == p->unit_count) {
        trailing_empty = false;
        break;
      }
      if (height > 0 && top > INT32_MAX - height) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "line block position exceeds the coordinate range");
      }
      top += height;
      if (unit == p->unit_count && p->unit_count
          && p->units[p->unit_count - 1].hard && !trailing_empty) {
        trailing_empty = true;
      }
      else if (unit == p->unit_count) {
        break;
      }
    } while (state->line_count < p->unit_count + 2);
    if (!state->line_count) state->line_count = 1;
  }
  state->starts[state->line_count] = p->unit_count;
  state->total_lines = state->line_count;
  if (p->options.max_lines && state->line_count > p->options.max_lines) {
    state->line_count = p->options.max_lines;
    state->more = true;
  }
  state->runs_bytes = (p->unit_count + state->line_count + 2) * sizeof *state->runs;
  state->stroke_room = p->kcount * (KASHIDA_COPIES_MAX + 4);
  if (p->kcount && width != GFNT_LAYOUT_UNBOUNDED) {
    const size_t per_line = (size_t)(width / p->kmin_adv) + 1;

    if (state->line_count <= (SIZE_MAX / 8) / per_line
        && state->line_count * per_line + 4 * p->kcount < state->stroke_room) {
      state->stroke_room = state->line_count * per_line + 4 * p->kcount;
    }
  }
  state->total_bytes = state->runs_bytes
      + (p->glyph_count + state->line_count * p->hyphen_glyphs + ELL_GLYPHS + 1
          + state->stroke_room) * sizeof *state->placed;
  state->lines = allocator->calloc_fn(allocator->ctx, state->line_count, sizeof *state->lines);
  state->storage = allocator->calloc_fn(allocator->ctx, state->total_bytes, 1);
  if (!state->lines || !state->storage) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the layout");
  }
  state->runs = (GFNT_LineRun *)state->storage;
  state->placed = (GFNT_PlacedGlyph *)(state->storage + state->runs_bytes);
  return GFNT_OK;
}

static void layout_state_free(LayoutState * state) {
  const GFNT_Allocator * allocator = state->allocator;

  for (size_t li = 0; state->lines && li < state->line_count; li++) {
    GFNT_Line * line = &state->lines[li];
    gfnt_lay_free_line_annotation_tree(allocator,
        (GFNT_LineAnnotation *)line->annotations, line->annotation_count);
  }

  allocator->free_fn(allocator->ctx, state->starts);
  allocator->free_fn(allocator->ctx, state->band_starts);
  allocator->free_fn(allocator->ctx, state->band_widths);
  allocator->free_fn(allocator->ctx, state->line_tops);
  allocator->free_fn(allocator->ctx, state->slices);
  allocator->free_fn(allocator->ctx, state->order);
  allocator->free_fn(allocator->ctx, state->slice_levels);
  allocator->free_fn(allocator->ctx, state->ew);
  allocator->free_fn(allocator->ctx, state->tabadv);
  allocator->free_fn(allocator->ctx, state->ruby_reclaim);
  allocator->free_fn(allocator->ctx, state->spacing_trim);
  allocator->free_fn(allocator->ctx, state->gjf);
  allocator->free_fn(allocator->ctx, state->gk);
  allocator->free_fn(allocator->ctx, state->kres);
  allocator->free_fn(allocator->ctx, state->spool);
  allocator->free_fn(allocator->ctx, state->hyphened);
  allocator->free_fn(allocator->ctx, state->cand_at);
  allocator->free_fn(allocator->ctx, state->cand_fit);
  allocator->free_fn(allocator->ctx, state->lines);
  allocator->free_fn(allocator->ctx, state->storage);
}

/** Transfer the completed buffers to the public result, then release scratch storage. */
static void layout_state_finish(LayoutState * state, GFNT_Layout * out,
    const GFNT_Paragraph * p, GFNT_F26Dot6 width, bool vertical, GFNT_F26Dot6 y,
    GFNT_F26Dot6 widest, size_t line_count, bool truncated, GFNT_Rect ink) {
  out->lines = state->lines;
  out->line_count = line_count;
  out->writing_mode = p->mode;
  out->width = vertical ? y : width == GFNT_LAYOUT_UNBOUNDED ? widest : width;
  out->height = vertical ? (width == GFNT_LAYOUT_UNBOUNDED ? widest : width) : y;
  out->truncated = truncated;
  out->ink = ink;
  out->allocator = state->allocator;
  out->storage_ = state->storage;
  out->paragraph_ = p;
  state->lines = NULL;
  state->storage = NULL;
  layout_state_free(state);
}

/** Assemble logical units into bidi-reordered visual run slices and establish the line box. */
static GFNT_Result build_line_slices(LineBuildState * state, GFNT_Error * error) {
  const GFNT_Paragraph * p = state->paragraph;
  const size_t ua = state->unit_start, ub = state->unit_end;
  GFNT_Line * line = state->line;
  size_t ns = 0;

  for (size_t j = ua; j < ub; j++) {
    const Unit * unit = &p->units[j];
    const PRun * run = &p->runs[unit->run];
    bool hang = j >= state->hang_from;
    uint8_t level = hang ? p->para_level : run->level;
    Slice * slice = ns ? &state->slices[ns - 1] : NULL;

    if (!slice || slice->pr != run || slice->level != level || slice->hang != hang) {
      slice = &state->slices[ns++];
      slice->pr = run;
      slice->src = p->glyphs;
      slice->ellipsis = false;
      slice->hyphen = false;
      slice->level = level;
      slice->hang = hang;
      slice->g_lo = unit->g_lo;
      slice->g_hi = unit->g_hi;
      slice->b_lo = p->cp_byte[unit->cp_start];
      slice->b_hi = p->cp_byte[unit->cp_end];
      slice->advance = 0;
    }
    if (p->cp_byte[unit->cp_start] < slice->b_lo) slice->b_lo = p->cp_byte[unit->cp_start];
    if (p->cp_byte[unit->cp_end] > slice->b_hi) slice->b_hi = p->cp_byte[unit->cp_end];
    if (unit->g_lo < slice->g_lo) slice->g_lo = unit->g_lo;
    if (unit->g_hi > slice->g_hi) slice->g_hi = unit->g_hi;
  }
  if (state->hyphenated) {
    const size_t at = p->cp_byte[p->units[ub - 1].cp_end];
    Slice * slice = &state->slices[ns++];

    for (size_t g = 0; g < state->hyphen_count; g++) {
      state->hyphen_glyphs[g].cluster = (uint32_t)at;
    }
    slice->pr = state->hyphen_run;
    slice->src = state->hyphen_glyphs;
    slice->ellipsis = false;
    slice->hyphen = true;
    slice->level = state->hyphen_run->level;
    slice->hang = false;
    slice->g_lo = 0;
    slice->g_hi = state->hyphen_count;
    slice->b_lo = slice->b_hi = at;
    slice->advance = 0;
  }
  if (state->truncated) {
    size_t at = ub > ua ? p->cp_byte[p->units[ub - 1].cp_end]
        : (ua < p->unit_count ? p->cp_byte[p->units[ua].cp_start] : p->cp_byte[p->length]);
    Slice * slice = &state->slices[ns++];

    for (size_t g = 0; g < state->ellipsis_count; g++) {
      state->ellipsis_glyphs[g].cluster = (uint32_t)at;
    }
    slice->pr = state->ellipsis_run;
    slice->src = state->ellipsis_glyphs;
    slice->ellipsis = true;
    slice->hyphen = false;
    slice->level = p->para_level;
    slice->hang = false;
    slice->g_lo = 0;
    slice->g_hi = state->ellipsis_count;
    slice->b_lo = slice->b_hi = at;
    slice->advance = 0;
  }
  for (size_t k = 0; k < ns; k++) state->levels[k] = state->slices[k].level;
  if (ns && guni_bidi_reorder(state->levels, ns, state->order, p->unit_count + 1) != GUNI_OK) {
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "the line could not be put in visual order");
  }
  line->start = ua < p->unit_count ? p->cp_byte[p->units[ua].cp_start] : p->cp_byte[p->length];
  line->end = ub > ua ? p->cp_byte[p->units[ub - 1].cp_end] : line->start;
  if (state->vertical) {
    line->box.x = state->measured->top_y;
    line->box.width = state->measured->thickness;
    line->ascent = line->descent = state->measured->thickness / 2;
  }
  else {
    line->box.y = state->measured->top_y;
    line->box.height = state->measured->gap < 0
        ? state->measured->ascent + state->measured->descent
        : state->measured->ascent + state->measured->descent + state->measured->gap;
    line->ascent = state->measured->ascent;
    line->descent = state->measured->descent;
    line->baseline = state->top_y + (state->measured->gap < 0 ? 0 : state->measured->gap / 2)
        + state->measured->ascent;
  }
  state->slice_count = ns;
  return GFNT_OK;
}

/** Record the phase results and apply physical alignment to every line. */
static void layout_process_finish(LayoutState * state, const GFNT_Paragraph * p,
    GFNT_F26Dot6 width, bool vertical, bool para_rtl, PGlyph * spool,
    size_t spool_len, size_t spool_cap, size_t run_used, size_t glyph_used,
    bool truncated, GFNT_F26Dot6 y, GFNT_F26Dot6 widest) {
  state->spool = spool;
  state->spool_len = spool_len;
  state->spool_cap = spool_cap;
  state->run_used = run_used;
  state->glyph_used = glyph_used;
  state->any_truncated = truncated;
  state->y = y;
  state->widest = widest;
  state->ink = gfnt_lay_align_lines(p, state->lines, state->line_count,
      state->band_starts, state->band_widths, width, widest, y, para_rtl, vertical);
}

/** Run measurement, justification, visual-run construction, placement, then alignment. */
static GFNT_Result layout_process_lines(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    bool vertical, bool para_rtl, LayoutState * state, GFNT_Error * error) {
  const GFNT_Allocator * allocator = state->allocator;
  size_t * starts = state->starts, * order = state->order;
  Slice * slices = state->slices;
  uint8_t * slice_levels = state->slice_levels, * gjf = state->gjf;
  GFNT_F26Dot6 * ew = state->ew, * tabadv = state->tabadv;
  uint32_t * gk = state->gk;
  KReshape * kres = state->kres;
  PGlyph * spool = state->spool;
  size_t spool_len = state->spool_len, spool_cap = state->spool_cap;
  bool * hyphened = state->hyphened;
  GFNT_Line * lines = state->lines;
  GFNT_LineRun * runs = state->runs;
  GFNT_PlacedGlyph * placed = state->placed;
  size_t line_count = state->line_count, total_lines = state->total_lines;
  GFNT_F26Dot6 * band_starts = state->band_starts, * band_widths = state->band_widths;
  GFNT_F26Dot6 * line_tops = state->line_tops;
  size_t run_used = state->run_used, glyph_used = state->glyph_used;
  bool more = state->more, any_truncated = false;
  GFNT_F26Dot6 y = 0, widest = 0;
  GFNT_Result r = GFNT_OK;

  for (size_t li = 0; li < line_count; li++) {
    size_t ua = starts[li];
    size_t ub = starts[li + 1];
    GFNT_Line * line = &lines[li];
    GFNT_F26Dot6 line_width = band_widths[li];
    GFNT_F26Dot6 line_top = line_tops[li];
    GFNT_F26Dot6 tab_origin = p->options.tabs_from_container ? band_starts[li] : 0;
    LayoutLineState line_state = gfnt_lay_measure_line(p, ua, ub, line_width, line_top, tab_origin,
        ew, tabadv, state->ruby_reclaim, state->spacing_trim);
    size_t hang_from;
    size_t ns = 0;
    PlacementState placement = {0};
    PGlyph ell_g[ELL_GLYPHS];
    size_t ell_n = 0;
    GFNT_F26Dot6 ell_w = 0, natural = 0, extra = 0, extra_rem = 0;
    PRun ell_run;
    PGlyph hy_g[ELL_GLYPHS];
    size_t hy_n = 0;
    GFNT_F26Dot6 hy_w = 0;
    PRun hy_run;
    bool hy_line = false;
    bool trunc_line = false;
    bool bounded = line_width != GFNT_LAYOUT_UNBOUNDED;

    line_state.start_offset = band_starts[li];

    if (line_state.invalid) {
      r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "line metrics exceed the supported layout range");
      break;
    }

    hang_from = line_state.hang_from;
    natural = line_state.natural;
    if (hyphened[li] && ub > ua) {
      // The line ends inside a word: a hyphen follows its last letter, and takes room.
      const PRun * last = &p->runs[p->units[ub - 1].run];

      r = gfnt_lay_shape_string(p, last->span, &p->hyphen_cp[last->span], 1, hy_g, &hy_n, &hy_w,
          &hy_run, gfnt_lay_hyphen_orientation(p), error);
      if (r != GFNT_OK) {
        state->spool = spool;
        state->spool_len = spool_len;
        state->spool_cap = spool_cap;
        return r;
      }
      hy_run.level = last->level;
      hy_line = true;
      natural += hy_w;
    }
    if (p->options.max_lines && p->ell_count && li + 1 == line_count
        && (more || (bounded && natural > line_width))) {
      size_t span = ub > ua ? p->runs[p->units[ub - 1].run].span
                            : (ua < p->unit_count ? p->runs[p->units[ua].run].span
                                                  : 0);

      r = gfnt_lay_shape_string(p, span, p->ell, p->ell_count, ell_g, &ell_n, &ell_w,
          &ell_run, -1, error);
      if (r != GFNT_OK) {
        state->spool = spool;
        state->spool_len = spool_len;
        state->spool_cap = spool_cap;
        return r;
      }
      if (bounded) {
        GFNT_F26Dot6 room = line_width - ell_w, used = 0;
        size_t k = ua;

        while (k < ub && used + ew[k] <= room) {
          used += ew[k];
          k++;
        }
        ub = k;
      }
      while (ub > ua && p->units[ub - 1].space) {
        ub--;
      }
      hang_from = ub;
      trunc_line = true;
      hy_line = false;   // the mark takes the hyphen's place
      any_truncated = true;
    }
    if (more && li + 1 == line_count) {
      any_truncated = true;
    }
    {
      JustificationState justification = {
          p, allocator, gjf, gk, kres, &spool, &spool_len, &spool_cap
      };

      r = gfnt_lay_justify_line(&justification, line, ua, ub, hang_from, natural,
          line_width, li, total_lines, bounded, trunc_line,
          &extra, &extra_rem, error);
      if (r != GFNT_OK) {
        state->spool = spool;
        state->spool_len = spool_len;
        state->spool_cap = spool_cap;
        return r;
      }
    }
    {
      LineBuildState build = {
        p, slices, slice_levels, order, line, ua, ub, hang_from, 0,
        hy_line, trunc_line, vertical, hy_g, ell_g, hy_n, ell_n,
        &hy_run, &ell_run, &line_state, line_top
      };

      r = build_line_slices(&build, error);
      if (r != GFNT_OK) {
        state->spool = spool;
        state->spool_len = spool_len;
        state->spool_cap = spool_cap;
        return r;
      }
      ns = build.slice_count;
    }
    placement.paragraph = p;
    placement.slices = slices;
    placement.order = order;
    placement.runs = runs;
    placement.run_used = &run_used;
    placement.placed = placed;
    placement.glyph_used = &glyph_used;
    placement.gjf = gjf;
    placement.gk = gk;
    placement.kres = kres;
    placement.spool = spool;
    placement.tabadv = tabadv;
    placement.ruby_reclaim = state->ruby_reclaim;
    placement.spacing_trim = state->spacing_trim;
    placement.unit_start = ua;
    placement.unit_end = ub;
    placement.extra = extra;
    placement.extra_rem = extra_rem;
    placement.vertical = vertical;
    placement.inline_mode = line_state.gap < 0;
    placement.line_ascent = line_state.ascent;
    placement.line_descent = line_state.descent;
    gfnt_lay_place_line(&placement, line, ns);
    if (placement.failed) {
      r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "line alignment exceeds the supported layout range");
      break;
    }
    line->truncated = trunc_line;
    line->offset = line_state.start_offset - placement.lead;
    if (line->extent > widest) {
      widest = line->extent;
    }
    {
      GFNT_F26Dot6 line_height = vertical ? line->box.width : line->box.height;
      if (line_top <= INT32_MAX - line_height && line_top + line_height > y) {
        y = line_top + line_height;
      }
    }
  }
  layout_process_finish(state, p, width, vertical, para_rtl, spool, spool_len,
      spool_cap, run_used, glyph_used, any_truncated, y, widest);
  return r;
}





GFNT_Result gfnt_paragraph_layout(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    GFNT_Layout * out, GFNT_Error * error) {
  return paragraph_layout_internal(p, width, NULL, out, error);
}

static GFNT_Result paragraph_layout_internal(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    const GFNT_LineConstraint * constraint, GFNT_Layout * out, GFNT_Error * error) {
  LayoutState state = {0};
  bool para_rtl;
  const bool vertical = p ? p->vertical : false;
  GFNT_Result r = GFNT_OK;

  gfnt_error_clear(error);
  if (!p || !out || width < 0 || (constraint && !constraint->band)) return GFNT_ERR_INVALID;
  if (!constraint && width != GFNT_LAYOUT_UNBOUNDED
      && (int64_t)width - p->options.text_indent > INT32_MAX) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "line indentation exceeds the supported coordinate range");
  }
  para_rtl = (p->para_level & 1) != 0;
  state.allocator = p->allocator;
  r = layout_state_allocate(&state, p, constraint ? GFNT_LAYOUT_UNBOUNDED : width,
      constraint, error);
  if (r != GFNT_OK) goto done;
  if (constraint && !state.more) {
    gfnt_lay_flow_wrap_optimize(p, constraint, &state);
    gfnt_lay_flow_avoid_short_last_line(p, constraint, &state);
  }
  r = layout_process_lines(p, constraint ? 0 : width, vertical, para_rtl, &state, error);
  if (r != GFNT_OK) goto done;
  r = gfnt_lay_add_line_annotations(p, state.lines, state.line_count, error);
  if (r != GFNT_OK) goto done;
  state.ink = (GFNT_Rect){0};
  bool ink_set = false;
  for (size_t li = 0; li < state.line_count; li++) {
    const GFNT_Rect * ink = &state.lines[li].ink;
    if (ink->width <= 0 || ink->height <= 0) continue;
    if (!ink_set) {
      state.ink = *ink;
      ink_set = true;
    }
    else if (!gfnt_lay_layout_union_rect(&state.ink, ink)) {
      r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, state.lines[li].start,
          GFNT_GLYPH_NONE, "layout ink bounds exceed the supported coordinate range");
      goto done;
    }
  }
  if (constraint) {
    width = 0;
    for (size_t li = 0; li < state.line_count; li++) {
      int64_t end = (int64_t)state.band_starts[li] + state.band_widths[li];
      if (end > width) width = (GFNT_F26Dot6)end;
    }
  }
  layout_state_finish(&state, out, p, width, vertical, state.y, state.widest,
      state.line_count, state.any_truncated, state.ink);
  return GFNT_OK;
done:
  layout_state_free(&state);
  return r;
}

GFNT_Result gfnt_paragraph_layout_flow(const GFNT_Paragraph * p,
    const GFNT_LineConstraint * constraint, GFNT_Layout * out, GFNT_Error * error) {
  if (!constraint) return GFNT_ERR_INVALID;
  return paragraph_layout_internal(p, GFNT_LAYOUT_UNBOUNDED, constraint, out, error);
}
