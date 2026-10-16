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
 * Text-decoration geometry.
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

typedef struct {
  int64_t start, end;
} DecorationInterval;

typedef struct {
  int64_t start, end;
  int64_t ink_start, ink_end, ink_cross_start, ink_cross_end;
  bool whitespace, has_ink;
} DecorationGlyph;

static bool paragraph_byte_boundary(const GFNT_Paragraph * p, size_t byte,
    size_t * cp_index) {
  size_t lo = 0, hi = p->length + 1;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (p->cp_byte[mid] < byte) lo = mid + 1;
    else hi = mid;
  }
  if (lo > p->length || p->cp_byte[lo] != byte) return false;
  if (cp_index) *cp_index = lo;
  return true;
}

static const Unit * paragraph_unit_for_cp(const GFNT_Paragraph * p, size_t cp,
    size_t * unit_index) {
  size_t lo = 0, hi = p->unit_count;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (p->units[mid].cp_end <= cp) lo = mid + 1;
    else hi = mid;
  }
  if (lo >= p->unit_count || cp < p->units[lo].cp_start) return NULL;
  if (unit_index) *unit_index = lo;
  return &p->units[lo];
}

static const PGlyph * paragraph_glyph_for_placed(const GFNT_Paragraph * p,
    const GFNT_LineRun * run, const GFNT_PlacedGlyph * placed, const Unit ** out_unit) {
  size_t cp, ui;
  const Unit * unit;
  const PRun * source_run;

  if (!paragraph_byte_boundary(p, placed->cluster, &cp) || cp >= p->length) return NULL;
  unit = paragraph_unit_for_cp(p, cp, &ui);
  if (!unit || unit->object || unit->tab) return NULL;
  source_run = &p->runs[unit->run];
  if (source_run->span != run->span || source_run->face != run->face) return NULL;
  for (size_t i = unit->g_lo; i < unit->g_hi; i++) {
    const PGlyph * glyph = &p->glyphs[i];
    if (glyph->glyph == placed->glyph && glyph->cluster == placed->cluster) {
      if (out_unit) *out_unit = unit;
      return glyph;
    }
  }
  return NULL;
}

static bool decoration_interval_before(const DecorationInterval * a,
    const DecorationInterval * b) {
  return a->start < b->start || (a->start == b->start && a->end < b->end);
}

GFNT_Result gfnt_paragraph_decoration_layout(const GFNT_Paragraph * p,
    const GFNT_Layout * layout, size_t start, size_t end, GFNT_DecorationLine line,
    GFNT_F26Dot6 offset, GFNT_F26Dot6 thickness, GFNT_DecorationSkipInk skip_ink,
    GFNT_DecorationLayout * out, GFNT_Error * error) {
  const GFNT_Allocator * allocator;
  size_t total_glyphs = 0, capacity, segment_count = 0;
  DecorationGlyph * glyphs = NULL;
  DecorationInterval * selected = NULL;
  DecorationInterval * gaps = NULL;
  GFNT_DecorationSegment * segments = NULL;

  if (!p || !layout || !out || !layout->allocator || layout->paragraph_ != p
      || end < start || end > p->cp_byte[p->length]
      || !paragraph_byte_boundary(p, start, NULL) || !paragraph_byte_boundary(p, end, NULL)
      || thickness <= 0 || (unsigned)line > GFNT_DECORATION_LINE_THROUGH
      || (unsigned)skip_ink > GFNT_DECORATION_SKIP_INK_ALL) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "invalid text decoration geometry request");
  }
  allocator = layout->allocator;
  if (start == end) {
    *out = (GFNT_DecorationLayout){NULL, 0, allocator, NULL};
    return GFNT_OK;
  }
  for (size_t li = 0; li < layout->line_count; li++) {
    const GFNT_Line * current = &layout->lines[li];
    for (size_t ri = 0; ri < current->run_count; ri++) {
      if (current->runs[ri].glyph_count > SIZE_MAX - total_glyphs) {
        return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
            "text decoration glyph count overflows");
      }
      total_glyphs += current->runs[ri].glyph_count;
    }
  }
  if (total_glyphs > (SIZE_MAX - layout->line_count - 1) / 2) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "text decoration segment count overflows");
  }
  capacity = total_glyphs * 2 + layout->line_count + 1;
  if (total_glyphs > SIZE_MAX / sizeof *glyphs || total_glyphs > SIZE_MAX / sizeof *selected
      || total_glyphs > SIZE_MAX / sizeof *gaps
      || capacity > SIZE_MAX / sizeof *segments) {
    return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
        "text decoration allocation size overflows");
  }
  glyphs = allocator->calloc_fn(allocator->ctx, total_glyphs ? total_glyphs : 1,
      sizeof *glyphs);
  selected = allocator->calloc_fn(allocator->ctx, total_glyphs ? total_glyphs : 1,
      sizeof *selected);
  gaps = allocator->calloc_fn(allocator->ctx, total_glyphs ? total_glyphs : 1,
      sizeof *gaps);
  segments = allocator->calloc_fn(allocator->ctx, capacity, sizeof *segments);
  if (!glyphs || !selected || !gaps || !segments) {
    allocator->free_fn(allocator->ctx, glyphs);
    allocator->free_fn(allocator->ctx, selected);
    allocator->free_fn(allocator->ctx, gaps);
    allocator->free_fn(allocator->ctx, segments);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for text decoration geometry");
  }

  for (size_t li = 0; li < layout->line_count; li++) {
    const GFNT_Line * current = &layout->lines[li];
    const bool vertical = layout->writing_mode != GFNT_WRITING_HORIZONTAL_TB;
    const int64_t cross = (int64_t)current->baseline + offset;
    const int64_t cross_end = cross + thickness;
    size_t count = 0, gap_count = 0;
    size_t selected_count = 0, merged_count = 0;
    int64_t line_first = INT64_MAX, line_last = INT64_MIN;

    if (cross < INT32_MIN || cross_end > INT32_MAX) {
      allocator->free_fn(allocator->ctx, glyphs);
      allocator->free_fn(allocator->ctx, selected);
      allocator->free_fn(allocator->ctx, gaps);
      allocator->free_fn(allocator->ctx, segments);
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "text decoration cross-axis position overflows");
    }

    for (size_t ri = 0; ri < current->run_count; ri++) {
      const GFNT_LineRun * run = &current->runs[ri];
      if (run->ellipsis) continue;
      for (size_t gi = 0; gi < run->glyph_count; gi++) {
        const GFNT_PlacedGlyph * placed = &run->glyphs[gi];
        const Unit * unit = NULL;
        const PGlyph * source;
        size_t cp;
        int64_t pen = vertical ? placed->pen_y : placed->pen_x;
        int64_t after = pen + placed->advance;
        DecorationGlyph * item;
        bool range_match = false;

        if (!paragraph_byte_boundary(p, placed->cluster, &cp) || cp >= p->length) continue;
        unit = paragraph_unit_for_cp(p, cp, NULL);
        source = paragraph_glyph_for_placed(p, run, placed, &unit);
        if (source && unit) {
          size_t unit_start = p->cp_byte[unit->cp_start];
          size_t unit_end = p->cp_byte[unit->cp_end];
          range_match = unit_start < end && unit_end > start;
        }
        else if (unit) {
          size_t unit_start = p->cp_byte[unit->cp_start];
          size_t unit_end = p->cp_byte[unit->cp_end];
          range_match = unit_start < end && unit_end > start;
        }
        else {
          range_match = placed->cluster >= start && placed->cluster < end;
        }
        if (!unit || !unit->space) {
          int64_t glyph_start = pen < after ? pen : after;
          int64_t glyph_end = pen > after ? pen : after;
          if (glyph_start < line_first) line_first = glyph_start;
          if (glyph_end > line_last) line_last = glyph_end;
        }
        if (!range_match) continue;
        item = &glyphs[count++];
        *item = (DecorationGlyph){0};
        item->start = pen < after ? pen : after;
        item->end = pen > after ? pen : after;
        item->whitespace = unit && unit->space;
        if (!source || !source->has_ink) continue;

        if (!vertical) {
          item->ink_start = (int64_t)placed->x + source->ix0;
          item->ink_end = (int64_t)placed->x + source->ix1;
          item->ink_cross_start = (int64_t)placed->y - source->iy1;
          item->ink_cross_end = (int64_t)placed->y - source->iy0;
        }
        else if (run->orientation == GFNT_ORIENT_RUN_UPRIGHT) {
          item->ink_start = (int64_t)placed->y - source->iy1;
          item->ink_end = (int64_t)placed->y - source->iy0;
          item->ink_cross_start = (int64_t)placed->x + source->ix0;
          item->ink_cross_end = (int64_t)placed->x + source->ix1;
        }
        else if (layout->writing_mode == GFNT_WRITING_SIDEWAYS_LR) {
          item->ink_start = (int64_t)placed->y - source->ix1;
          item->ink_end = (int64_t)placed->y - source->ix0;
          item->ink_cross_start = (int64_t)placed->x - source->iy1;
          item->ink_cross_end = (int64_t)placed->x - source->iy0;
        }
        else {
          item->ink_start = (int64_t)placed->y + source->ix0;
          item->ink_end = (int64_t)placed->y + source->ix1;
          item->ink_cross_start = (int64_t)placed->x + source->iy0;
          item->ink_cross_end = (int64_t)placed->x + source->iy1;
        }
        item->has_ink = item->ink_start < item->ink_end
            && item->ink_cross_start < cross_end && item->ink_cross_end > cross;
      }
    }
    for (size_t i = 0; i < count; i++) {
      const DecorationGlyph * item = &glyphs[i];
      if (item->start >= item->end
          || (item->whitespace
              && (line_first == INT64_MAX || item->start < line_first || item->end > line_last))) {
        continue;
      }
      selected[selected_count++] = (DecorationInterval){item->start, item->end};
    }
    if (!selected_count) continue;
    for (size_t i = 1; i < selected_count; i++) {
      DecorationInterval value = selected[i];
      size_t j = i;
      while (j && decoration_interval_before(&value, &selected[j - 1])) {
        selected[j] = selected[j - 1];
        j--;
      }
      selected[j] = value;
    }
    for (size_t i = 0; i < selected_count; i++) {
      if (!merged_count || selected[i].start > selected[merged_count - 1].end) {
        selected[merged_count++] = selected[i];
      }
      else if (selected[i].end > selected[merged_count - 1].end) {
        selected[merged_count - 1].end = selected[i].end;
      }
    }
    for (size_t i = 0; i < merged_count; i++) {
      if (selected[i].start < INT32_MIN || selected[i].end > INT32_MAX
          || selected[i].end - selected[i].start > INT32_MAX) {
        allocator->free_fn(allocator->ctx, glyphs);
        allocator->free_fn(allocator->ctx, selected);
        allocator->free_fn(allocator->ctx, gaps);
        allocator->free_fn(allocator->ctx, segments);
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "text decoration line extent overflows");
      }
    }

    if (skip_ink != GFNT_DECORATION_SKIP_INK_NONE
        && line != GFNT_DECORATION_LINE_THROUGH) {
      const int64_t clearance = thickness > 64 ? thickness : 64;
      for (size_t i = 0; i < count; i++) {
        if (glyphs[i].has_ink) {
          gaps[gap_count++] = (DecorationInterval){
              glyphs[i].ink_start - clearance, glyphs[i].ink_end + clearance};
        }
      }
      for (size_t i = 1; i < gap_count; i++) {
        DecorationInterval value = gaps[i];
        size_t j = i;
        while (j && decoration_interval_before(&value, &gaps[j - 1])) {
          gaps[j] = gaps[j - 1];
          j--;
        }
        gaps[j] = value;
      }
    }

    for (size_t r = 0; r < merged_count; r++) {
      int64_t first = selected[r].start, last = selected[r].end, cursor = first;
      for (size_t i = 0; i <= gap_count; i++) {
        int64_t gap_start = i < gap_count ? gaps[i].start : last;
        int64_t gap_end = i < gap_count ? gaps[i].end : last;
        if (gap_start < first) gap_start = first;
        if (gap_start > last) gap_start = last;
        if (gap_end > last) gap_end = last;
        if (gap_start > cursor) {
          GFNT_Rect box = vertical
              ? (GFNT_Rect){(GFNT_F26Dot6)cross, (GFNT_F26Dot6)cursor,
                  thickness, (GFNT_F26Dot6)(gap_start - cursor)}
              : (GFNT_Rect){(GFNT_F26Dot6)cursor, (GFNT_F26Dot6)cross,
                  (GFNT_F26Dot6)(gap_start - cursor), thickness};
          segments[segment_count++] = (GFNT_DecorationSegment){li, box};
        }
        if (gap_end > cursor) cursor = gap_end;
        if (cursor >= last) break;
      }
    }
  }
  allocator->free_fn(allocator->ctx, glyphs);
  allocator->free_fn(allocator->ctx, selected);
  allocator->free_fn(allocator->ctx, gaps);
  if (segment_count == 0) {
    allocator->free_fn(allocator->ctx, segments);
    segments = NULL;
  }
  else {
    GFNT_DecorationSegment * compact = allocator->realloc_fn(allocator->ctx,
        segments, segment_count * sizeof *segments);
    if (compact) segments = compact;
  }
  *out = (GFNT_DecorationLayout){segments, segment_count, allocator, segments};
  return GFNT_OK;
}

void gfnt_decoration_layout_free(GFNT_DecorationLayout * layout) {
  if (!layout || !layout->allocator) return;
  layout->allocator->free_fn(layout->allocator->ctx, (void *)layout->storage_);
  memset(layout, 0, sizeof *layout);
}
