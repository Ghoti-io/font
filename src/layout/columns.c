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
 * Laying a paragraph out in columns.
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

static bool column_shift_coord(GFNT_F26Dot6 * value, GFNT_F26Dot6 delta) {
  int64_t shifted = (int64_t)*value + delta;
  if (shifted < INT32_MIN || shifted > INT32_MAX) return false;
  *value = (GFNT_F26Dot6)shifted;
  return true;
}

static bool column_shift_rect(GFNT_Rect * rect, GFNT_F26Dot6 dx, GFNT_F26Dot6 dy) {
  int64_t x = (int64_t)rect->x + dx;
  int64_t y = (int64_t)rect->y + dy;
  int64_t right = x + rect->width;
  int64_t bottom = y + rect->height;
  if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX
      || rect->width < 0 || rect->height < 0
      || right < INT32_MIN || right > INT32_MAX
      || bottom < INT32_MIN || bottom > INT32_MAX) return false;
  rect->x = (GFNT_F26Dot6)x;
  rect->y = (GFNT_F26Dot6)y;
  return true;
}

bool gfnt_lay_layout_union_rect(GFNT_Rect * bounds, const GFNT_Rect * rect) {
  int64_t left = bounds->x < rect->x ? bounds->x : rect->x;
  int64_t top = bounds->y < rect->y ? bounds->y : rect->y;
  int64_t right_a = (int64_t)bounds->x + bounds->width;
  int64_t right_b = (int64_t)rect->x + rect->width;
  int64_t bottom_a = (int64_t)bounds->y + bounds->height;
  int64_t bottom_b = (int64_t)rect->y + rect->height;
  int64_t right = right_a > right_b ? right_a : right_b;
  int64_t bottom = bottom_a > bottom_b ? bottom_a : bottom_b;
  int64_t width = right - left, height = bottom - top;
  if (left < INT32_MIN || left > INT32_MAX || top < INT32_MIN || top > INT32_MAX
      || right < INT32_MIN || right > INT32_MAX
      || bottom < INT32_MIN || bottom > INT32_MAX
      || width < 0 || width > INT32_MAX || height < 0 || height > INT32_MAX) return false;
  *bounds = (GFNT_Rect){(GFNT_F26Dot6)left, (GFNT_F26Dot6)top,
      (GFNT_F26Dot6)width, (GFNT_F26Dot6)height};
  return true;
}

static bool column_shift_annotation(GFNT_LineAnnotation * annotation,
    GFNT_F26Dot6 dx, GFNT_F26Dot6 dy, bool vertical) {
  if (!column_shift_rect(&annotation->box, dx, dy)) return false;
  GFNT_LineRun * runs = (GFNT_LineRun *)annotation->runs;
  for (size_t ri = 0; ri < annotation->run_count; ri++) {
    GFNT_LineRun * run = &runs[ri];
    GFNT_PlacedGlyph * glyphs = (GFNT_PlacedGlyph *)run->glyphs;
    if (!column_shift_coord(&run->pos, vertical ? dy : dx)) return false;
    for (size_t gi = 0; gi < run->glyph_count; gi++) {
      if (!column_shift_coord(&glyphs[gi].pen_x, dx)
          || !column_shift_coord(&glyphs[gi].pen_y, dy)
          || !column_shift_coord(&glyphs[gi].x, dx)
          || !column_shift_coord(&glyphs[gi].y, dy)) return false;
    }
  }
  GFNT_LineAnnotation * children = (GFNT_LineAnnotation *)annotation->children;
  for (size_t ci = 0; ci < annotation->child_count; ci++) {
    if (!column_shift_annotation(&children[ci], dx, dy, vertical)) return false;
  }
  return true;
}

static GFNT_F26Dot6 column_line_block_size(const GFNT_Line * line, bool vertical) {
  return vertical ? line->box.width : line->box.height;
}

/** Partition whole line boxes into columns, preserving order and making progress. */
static size_t column_partition(const GFNT_Layout * layout, bool vertical,
    size_t columns, GFNT_F26Dot6 extent, size_t orphans, size_t widows, size_t * starts) {
  size_t at = 0;
  starts[0] = 0;
  for (size_t ci = 0; ci < columns; ci++) {
    const size_t begin = at;
    int64_t used = 0;
    while (at < layout->line_count) {
      GFNT_F26Dot6 height = column_line_block_size(&layout->lines[at], vertical);
      /* Keep the subtraction in range even when accumulated line heights
       * approach the maximum representable block extent. */
      if ((used >= extent || height > extent - used) && at > begin) break;
      used += height;
      at++;
    }
    if (at < layout->line_count) {
      while (layout->line_count - at < widows && at > begin
          && at - begin > orphans) {
        at--;
        used -= column_line_block_size(&layout->lines[at], vertical);
      }
      /* If both constraints cannot be met, keep the requested orphan count. This matches
       * Chromium's fragmentation choice for a 3/1 split when four lines cannot satisfy
       * orphans: 3 and widows: 2 in a two-column row. */
      if (at == begin && at < layout->line_count) {
        used += column_line_block_size(&layout->lines[at], vertical);
        at++;
      }
    }
    starts[ci + 1] = at;
    if (at == layout->line_count) {
      for (size_t rest = ci + 1; rest < columns; rest++) starts[rest + 1] = at;
      break;
    }
  }
  return at;
}

void gfnt_column_layout_free(GFNT_ColumnLayout * layout) {
  if (!layout) return;
  const GFNT_Allocator * allocator = layout->layout.allocator;
  if (allocator && layout->storage_) allocator->free_fn(allocator->ctx, layout->storage_);
  gfnt_layout_free(&layout->layout);
  memset(layout, 0, sizeof *layout);
}

GFNT_Result gfnt_paragraph_layout_columns(const GFNT_Paragraph * p,
    const GFNT_ColumnOptions * options, GFNT_ColumnLayout * out, GFNT_Error * error) {
  GFNT_ColumnLayout result = {0};
  GFNT_Layout full = {0};
  const GFNT_Allocator * allocator;
  size_t * starts;
  size_t used, total_lines, orphans, widows;
  size_t next_byte;
  bool complete;
  GFNT_F26Dot6 extent;
  bool vertical;
  GFNT_Result r;

  gfnt_error_clear(error);
  if (!p || !options || !out || options->inline_size <= 0 || options->inline_gap < 0
      || options->block_extent <= 0
      || !options->count || options->count > SIZE_MAX / sizeof(size_t) - 1
      || (unsigned)options->fill > GFNT_COLUMN_FILL_AUTO
      || (unsigned)options->overflow > GFNT_COLUMN_OVERFLOW_ERROR) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "invalid column layout options or truncated paragraph");
  }
  vertical = p->vertical;
  if (options->count > (size_t)(INT32_MAX / options->inline_size)
      || (int64_t)options->count * options->inline_size
          + (int64_t)(options->count - 1) * options->inline_gap > INT32_MAX) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "column layout exceeds the supported coordinate range");
  }
  allocator = p->allocator;
  r = gfnt_paragraph_layout(p, options->inline_size, &full, error);
  if (r != GFNT_OK) return r;
  total_lines = full.line_count;
  if (full.truncated) {
    gfnt_layout_free(&full);
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "column layout cannot continue from a truncated paragraph");
  }
  starts = allocator->calloc_fn(allocator->ctx, options->count + 1, sizeof *starts);
  if (!starts) {
    gfnt_layout_free(&full);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "could not allocate column line ranges");
  }
  orphans = options->orphans ? options->orphans : 2;
  widows = options->widows ? options->widows : 2;
  extent = options->block_extent;
  if (options->fill == GFNT_COLUMN_FILL_BALANCE && full.line_count) {
    size_t high_fit = column_partition(&full, vertical, options->count, extent,
        orphans, widows, starts);
    if (high_fit == full.line_count) {
      /* column_partition() deliberately places an oversized first line to make
       * progress. Such a line therefore appears to fit even when the candidate
       * extent is smaller than its box. Keep the balance search's lower bound at
       * least the largest line box so this progress rule cannot masquerade as a
       * smaller valid balanced extent. The configured extent remains the upper
       * bound and still controls the overflow policy below. */
      GFNT_F26Dot6 low = 1, high = extent;
      for (size_t li = 0; li < full.line_count; li++) {
        GFNT_F26Dot6 line_extent = column_line_block_size(&full.lines[li], vertical);
        if (line_extent > low) low = line_extent;
      }
      if (low <= high) {
        while (low < high) {
          GFNT_F26Dot6 mid = low + (high - low) / 2;
          size_t fit = column_partition(&full, vertical, options->count, mid,
              orphans, widows, starts);
          if (fit == full.line_count) high = mid;
          else low = mid + 1;
        }
        extent = low;
      }
    }
  }
  used = column_partition(&full, vertical, options->count, extent, orphans, widows, starts);
  if (options->overflow == GFNT_COLUMN_OVERFLOW_ERROR) {
    for (size_t li = 0; li < used; li++) {
      if (column_line_block_size(&full.lines[li], vertical) > options->block_extent) {
        size_t line_start = full.lines[li].start;
        allocator->free_fn(allocator->ctx, starts);
        gfnt_layout_free(&full);
        return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, line_start,
            GFNT_GLYPH_NONE, "a placed line is taller than the column block extent");
      }
    }
  }
  complete = used == total_lines && !full.truncated;
  next_byte = complete ? p->cp_byte[p->length]
      : (used < total_lines ? full.lines[used].start : p->cp_byte[p->length]);
  for (size_t ci = 0; ci < options->count; ci++) {
    size_t begin = starts[ci], end = starts[ci + 1];
    if (begin == end) continue;
    GFNT_Line * first = &full.lines[begin];
    GFNT_F26Dot6 dx = 0, dy = 0;
    int64_t shifted_x = 0, shifted_y = 0;
    GFNT_F26Dot6 gfnt_lay_inline_shift = (GFNT_F26Dot6)((int64_t)ci
        * ((int64_t)options->inline_size + options->inline_gap));
    if (vertical) {
      shifted_y = gfnt_lay_inline_shift;
      shifted_x = p->mode == GFNT_WRITING_VERTICAL_RL
          ? (int64_t)options->block_extent - first->box.x - first->box.width
          : -(int64_t)first->box.x;
    }
    else {
      /* CSS multi-column progression follows the paragraph's inline direction:
       * an RTL paragraph places its first column at the right edge. */
      shifted_x = (p->para_level & 1)
          ? (int64_t)options->count * options->inline_size
              + (int64_t)(options->count - 1) * options->inline_gap
              - options->inline_size - gfnt_lay_inline_shift
          : gfnt_lay_inline_shift;
      shifted_y = -(int64_t)first->box.y;
    }
    if (shifted_x < INT32_MIN || shifted_x > INT32_MAX
        || shifted_y < INT32_MIN || shifted_y > INT32_MAX) {
      allocator->free_fn(allocator->ctx, starts);
      gfnt_layout_free(&full);
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, first->start,
          GFNT_GLYPH_NONE, "column placement exceeds the supported coordinate range");
    }
    dx = (GFNT_F26Dot6)shifted_x;
    dy = (GFNT_F26Dot6)shifted_y;
    for (size_t li = begin; li < end; li++) {
      GFNT_Line * line = &full.lines[li];
      if (!column_shift_rect(&line->box, dx, dy)
          || !column_shift_rect(&line->ink, dx, dy)
          || !column_shift_coord(&line->offset, vertical ? dy : dx)
          || !column_shift_coord(&line->baseline, vertical ? dx : dy)) {
        allocator->free_fn(allocator->ctx, starts);
        gfnt_layout_free(&full);
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, line->start,
            GFNT_GLYPH_NONE, "column placement exceeds the supported coordinate range");
      }
      GFNT_LineRun * runs = (GFNT_LineRun *)line->runs;
      for (size_t ri = 0; ri < line->run_count; ri++) {
        GFNT_LineRun * run = &runs[ri];
        GFNT_PlacedGlyph * glyphs = (GFNT_PlacedGlyph *)run->glyphs;
        if (!column_shift_coord(&run->pos, vertical ? dy : dx)) {
          allocator->free_fn(allocator->ctx, starts);
          gfnt_layout_free(&full);
          return gfnt_error_set(error, GFNT_ERR_INVALID, 0, line->start,
              GFNT_GLYPH_NONE, "column placement exceeds the supported coordinate range");
        }
        for (size_t gi = 0; gi < run->glyph_count; gi++) {
          if (!column_shift_coord(&glyphs[gi].pen_x, dx)
              || !column_shift_coord(&glyphs[gi].pen_y, dy)
              || !column_shift_coord(&glyphs[gi].x, dx)
              || !column_shift_coord(&glyphs[gi].y, dy)) {
            allocator->free_fn(allocator->ctx, starts);
            gfnt_layout_free(&full);
            return gfnt_error_set(error, GFNT_ERR_INVALID, 0, line->start,
                glyphs[gi].glyph, "column placement exceeds the supported coordinate range");
          }
        }
      }
      GFNT_LineAnnotation * annotations = (GFNT_LineAnnotation *)line->annotations;
      for (size_t ai = 0; ai < line->annotation_count; ai++) {
        if (!column_shift_annotation(&annotations[ai], dx, dy, vertical)) {
          allocator->free_fn(allocator->ctx, starts);
          gfnt_layout_free(&full);
          return gfnt_error_set(error, GFNT_ERR_INVALID, 0, line->start,
              GFNT_GLYPH_NONE, "column annotation exceeds the supported coordinate range");
        }
      }
    }
  }
  for (size_t li = used; li < total_lines; li++) {
    GFNT_Line * line = &full.lines[li];
    gfnt_lay_free_line_annotation_tree(allocator,
        (GFNT_LineAnnotation *)line->annotations, line->annotation_count);
    line->annotations = NULL;
    line->annotation_count = 0;
  }
  full.line_count = used;
  GFNT_F26Dot6 total_inline = (GFNT_F26Dot6)((int64_t)options->inline_size * options->count
      + (int64_t)options->inline_gap * (options->count - 1));
  full.width = vertical ? options->block_extent : total_inline;
  full.height = vertical ? options->inline_size : options->block_extent;
  full.ink = (GFNT_Rect){0};
  bool ink_set = false;
  for (size_t li = 0; li < used; li++) {
    GFNT_Rect ink = full.lines[li].ink;
    if (ink.width <= 0 || ink.height <= 0) continue;
    if (!ink_set) {
      full.ink = ink;
      ink_set = true;
    }
    else {
      if (!gfnt_lay_layout_union_rect(&full.ink, &ink)) {
        allocator->free_fn(allocator->ctx, starts);
        gfnt_layout_free(&full);
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "column ink bounds exceed the supported coordinate range");
      }
    }
  }
  result.layout = full;
  result.column_count = options->count;
  result.column_line_starts = starts;
  result.storage_ = starts;
  result.complete = complete;
  result.next_byte = next_byte;
  *out = result;
  return GFNT_OK;
}
