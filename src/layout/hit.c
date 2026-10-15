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
 * Hit testing, carets and selection rectangles.
 *
 * Everything is built from the *boxes* of a line: one per grapheme, in visual
 * order, with the bytes it stands for and the range it covers along the line.
 * In vertical text the line is a column and the range is a span of y; for
 * sideways-lr, whose lines read upward, the boxes are worked out in the frame
 * of sideways-rl (coordinates measured from the bottom) and turned back at the
 * edges. A cluster of
 * several graphemes (a ligature) is divided at the font's `GDEF` ligature
 * carets when it has one per component and evenly otherwise. A newline is not a
 * box, so nothing lands after it.
 */

#include <ghoti.io/font/layout.h>
#include <ghoti.io/unicode/break.h>
#include <string.h>
#include "layout_internal.h"

typedef struct {
  size_t lo, hi;        // bytes
  GFNT_F26Dot6 x0, x1;  // along the line, in the frame where it reads toward larger values; x0 <= x1
  bool rtl;
} Box;

static bool is_vertical(const GFNT_Layout * l) {
  return l->writing_mode != GFNT_WRITING_HORIZONTAL_TB;
}

static bool is_flipped(const GFNT_Layout * l) {
  return l->writing_mode == GFNT_WRITING_SIDEWAYS_LR;
}

/** A coordinate along the line, turned to the frame the boxes are in (and back: it is its own inverse). */
static GFNT_F26Dot6 canon(const GFNT_Layout * l, GFNT_F26Dot6 c) {
  return is_flipped(l) ? l->height - c : c;
}

static bool is_newline(uint32_t cp) {
  return cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x85
      || cp == 0x2028 || cp == 0x2029;
}

/** The index of the code point that starts at byte @p b, or the count. */
static size_t cp_of_byte(const GFNT_Paragraph * p, size_t b) {
  size_t lo = 0, hi = p->length;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;

    if (p->cp_byte[mid] < b) {
      lo = mid + 1;
    }
    else {
      hi = mid;
    }
  }
  return lo;
}

/** Where a line's text ends, not counting a newline that ended it. */
static size_t text_end(const GFNT_Paragraph * p, const GFNT_Line * line) {
  size_t c = cp_of_byte(p, line->end);

  if (c > cp_of_byte(p, line->start) && is_newline(p->cps[c - 1])) {
    return p->cp_byte[c - 1];
  }
  return line->end;
}

static GFNT_F26Dot6 mul_div(GFNT_F26Dot6 a, size_t num, size_t den) {
  return (GFNT_F26Dot6)((int64_t)a * (int64_t)num / (int64_t)den);
}

/**
 * Append the boxes of one glyph group (glyphs [g0, g1) of a run, one cluster)
 * covering the bytes [lo, hi). Returns the new count.
 */
static size_t add_group(const GFNT_Paragraph * p, const GFNT_Layout * layout,
    const GFNT_LineRun * run, size_t g0, size_t g1, size_t lo, size_t hi, Box * boxes, size_t n) {
  const GFNT_PlacedGlyph * first = &run->glyphs[g0];
  GFNT_F26Dot6 x0 = canon(layout, is_vertical(layout) ? first->pen_y : first->pen_x), x1 = x0;
  bool rtl = (run->level & 1) != 0;
  size_t c0 = cp_of_byte(p, lo), c1 = cp_of_byte(p, hi);
  size_t graphemes = 0;
  size_t cuts[64];   // grapheme starts, as code point indices
  GFNT_F26Dot6 edge[65];
  GUNI_BreakOptions bo;

  for (size_t g = g0; g < g1; g++) {
    x1 += run->glyphs[g].advance;
  }
  if (c1 > c0 && c1 - c0 == 1 && is_newline(p->cps[c0])) {
    return n;   // a newline is not a place
  }
  memset(&bo, 0, sizeof bo);
  bo.kind = GUNI_BREAK_GRAPHEME;
  for (size_t c = c0; c < c1 && graphemes < 64; c++) {
    if (c == c0 || (p->cp_byte[c] > p->cp_byte[c - 1]
            && guni_break_at_codepoints(&bo, p->cps, p->length, c))) {
      cuts[graphemes++] = c;
    }
  }
  if (graphemes <= 1) {
    boxes[n].lo = lo;
    boxes[n].hi = hi;
    boxes[n].x0 = x0;
    boxes[n].x1 = x1;
    boxes[n].rtl = rtl;
    return n + 1;
  }
  // The visual edges between the graphemes: the font's carets when it gives
  // one fewer than the graphemes for a lone glyph, an even division otherwise.
  edge[0] = x0;
  edge[graphemes] = x1;
  {
    int32_t carets[64];
    size_t have = 0;

    if (g1 - g0 == 1 && run->face && run->orientation != GFNT_ORIENT_RUN_UPRIGHT) {
      const GFNT_TextStyle * style = &p->styles[run->span];
      uint16_t upem = 0;

      have = gfnt_face_ligature_carets(run->face, first->glyph, 0,
          style->variation, 0, carets, 64);
      if (have == graphemes - 1 && gfnt_face_units_per_em(run->face, &upem, NULL)
              == GFNT_OK && upem) {
        GFNT_F16Dot16 scale = (GFNT_F16Dot16)(((int64_t)style->size * 1024
            + upem / 2) / upem);

        for (size_t i = 0; i < have; i++) {
          edge[i + 1] = x0 + gfnt_units_to_pixels(carets[i], scale);
        }
      }
      else {
        have = 0;
      }
    }
    if (!have) {
      for (size_t i = 1; i < graphemes; i++) {
        edge[i] = x0 + mul_div(x1 - x0, i, graphemes);
      }
    }
  }
  for (size_t v = 0; v < graphemes; v++) {
    size_t j = rtl ? graphemes - 1 - v : v;   // the logical index

    boxes[n].lo = p->cp_byte[cuts[j]];
    boxes[n].hi = j + 1 < graphemes ? p->cp_byte[cuts[j + 1]] : hi;
    boxes[n].x0 = edge[v];
    boxes[n].x1 = edge[v + 1];
    boxes[n].rtl = rtl;
    n++;
  }
  return n;
}

/** The boxes of a line, in visual order. Free with the paragraph's allocator. */
static GFNT_Result line_boxes(const GFNT_Paragraph * p, const GFNT_Layout * layout,
    const GFNT_Line * line, Box ** out, size_t * count) {
  const GFNT_Allocator * allocator = p->allocator;
  Box * boxes = allocator->calloc_fn(allocator->ctx, p->length + 2, sizeof *boxes);
  size_t n = 0;

  if (!boxes) {
    return GFNT_ERR_OOM;
  }
  for (size_t k = 0; k < line->run_count; k++) {
    const GFNT_LineRun * run = &line->runs[k];
    bool rtl = (run->level & 1) != 0;
    size_t g = 0;

    while (g < run->glyph_count) {
      size_t e = g + 1;
      size_t lo = run->glyphs[g].cluster, hi;

      while (e < run->glyph_count && run->glyphs[e].cluster == lo) {
        e++;
      }
      if (rtl) {
        // Visual order: clusters fall along the array, so the previous
        // group's cluster is where this one ends.
        hi = g == 0 ? run->end : run->glyphs[g - 1].cluster;
      }
      else {
        hi = e < run->glyph_count ? run->glyphs[e].cluster : run->end;
      }
      if (hi > lo) {
        n = add_group(p, layout, run, g, e, lo, hi, boxes, n);
      }
      g = e;
    }
  }
  *out = boxes;
  *count = n;
  return GFNT_OK;
}

/** Line metrics used for base-text selection exclude separately painted annotation tracks. */
static void base_line_metrics(const GFNT_Paragraph * p, const GFNT_Line * line,
    GFNT_F26Dot6 * ascent, GFNT_F26Dot6 * descent) {
  int64_t over = 0, under = 0;
  *ascent = line->ascent;
  *descent = line->descent;
  for (size_t i = 0; i < p->annotation_count; i++) {
    const PAnnotation * annotation = &p->annotations[i];
    const size_t start = p->cp_byte[annotation->cp_start];
    const size_t end = p->cp_byte[annotation->cp_end];
    if (end <= line->start || start >= line->end) continue;
    const int64_t extent = (int64_t)annotation->ascent + annotation->descent;
    if (annotation->descriptor.position == GFNT_ANNOTATION_UNDER) under += extent;
    else over += extent;
  }
  /* Line metrics include annotation tracks. Recover the base metrics with
   * signed arithmetic: a large baseline shift can legitimately put either
   * edge beyond the line box, and clamping would enlarge selection geometry. */
  *ascent -= (GFNT_F26Dot6)over;
  *descent -= (GFNT_F26Dot6)under;
}

static bool valid(const GFNT_Paragraph * p, const GFNT_Layout * l) {
  return p && l && l->lines && l->line_count;
}

static bool annotation_contains(const GFNT_LineAnnotation * a, GFNT_F26Dot6 x, GFNT_F26Dot6 y) {
  if (x >= a->box.x && x < a->box.x + a->box.width
      && y >= a->box.y && y < a->box.y + a->box.height) return true;
  for (size_t i = 0; i < a->child_count; i++) {
    if (annotation_contains(&a->children[i], x, y)) return true;
  }
  return false;
}

static const GFNT_LineAnnotation * annotation_at(const GFNT_LineAnnotation * annotations,
    size_t count, GFNT_F26Dot6 x, GFNT_F26Dot6 y) {
  for (size_t i = 0; i < count; i++) {
    const GFNT_LineAnnotation * a = &annotations[i];
    if (annotation_contains(a, x, y)) return a;
  }
  return NULL;
}

GFNT_Result gfnt_layout_hit_test(const GFNT_Paragraph * p, const GFNT_Layout * l,
    GFNT_F26Dot6 x, GFNT_F26Dot6 y, GFNT_Hit * out) {
  size_t li = 0;
  Box * boxes = NULL;
  size_t n = 0;
  GFNT_Hit hit;
  GFNT_Result r;

  if (!valid(p, l) || !out) {
    return GFNT_ERR_INVALID;
  }
  if (!is_vertical(l)) {
    while (li + 1 < l->line_count && y >= l->lines[li].box.y + l->lines[li].box.height) {
      li++;
    }
  }
  else if (l->writing_mode == GFNT_WRITING_VERTICAL_RL
      || l->writing_mode == GFNT_WRITING_SIDEWAYS_RL) {
    // Columns read from the right: the next one is further left.
    while (li + 1 < l->line_count && x < l->lines[li].box.x) {
      li++;
    }
  }
  else {
    while (li + 1 < l->line_count && x >= l->lines[li].box.x + l->lines[li].box.width) {
      li++;
    }
  }
  r = line_boxes(p, l, &l->lines[li], &boxes, &n);
  if (r != GFNT_OK) {
    return r;
  }
  hit.line = li;
  hit.offset = l->lines[li].start;
  hit.affinity = GFNT_AFFINITY_DOWNSTREAM;
  {
    const GFNT_LineAnnotation * annotation = annotation_at(l->lines[li].annotations,
        l->lines[li].annotation_count, x, y);
    if (annotation) {
    const GFNT_Rect box = annotation->box;
      const GFNT_F26Dot6 along = is_vertical(l) ? y : x;
      const GFNT_F26Dot6 start = is_vertical(l) ? box.y : box.x;
      const GFNT_F26Dot6 extent = is_vertical(l) ? box.height : box.width;
      /* Nested annotation offsets belong to the parent annotation text. Until that text has
       * a source mapping, a hit on any descendant resolves to the top-level base range. */
      hit.offset = along < start + extent / 2 ? annotation->base_start : annotation->base_end;
      hit.affinity = hit.offset == annotation->base_start
          ? GFNT_AFFINITY_DOWNSTREAM : GFNT_AFFINITY_UPSTREAM;
      p->allocator->free_fn(p->allocator->ctx, boxes);
      *out = hit;
      return GFNT_OK;
    }
  }
  if (n) {
    size_t pick = n;
    bool leading;

    x = canon(l, is_vertical(l) ? y : x);
    for (size_t i = 0; i < n; i++) {
      if (x < boxes[i].x1) {
        pick = i;
        break;
      }
    }
    if (pick == n) {
      pick = n - 1;
      leading = false;   // past the last box: its far visual edge
    }
    else if (x < boxes[pick].x0) {
      leading = true;    // before the first box (or in a gap): its near edge
    }
    else {
      leading = x - boxes[pick].x0 < boxes[pick].x1 - x;   // the left half
    }
    // `leading` here means the visual left; the logical start is the left
    // edge in a left-to-right box and the right edge in a right-to-left one.
    {
      const Box * b = &boxes[pick];
      bool logical_start = b->rtl ? !leading : leading;

      hit.offset = logical_start ? b->lo : b->hi;
      hit.affinity = logical_start ? GFNT_AFFINITY_DOWNSTREAM
                                   : GFNT_AFFINITY_UPSTREAM;
    }
  }
  p->allocator->free_fn(p->allocator->ctx, boxes);
  *out = hit;
  return GFNT_OK;
}

GFNT_Result gfnt_layout_caret(const GFNT_Paragraph * p, const GFNT_Layout * l,
    size_t offset, GFNT_Affinity affinity, GFNT_Caret * out) {
  size_t li = (size_t)-1;
  Box * boxes = NULL;
  size_t n = 0;
  GFNT_Caret caret;
  GFNT_Result r;
  size_t total;

  if (!valid(p, l) || !out || (unsigned)affinity > GFNT_AFFINITY_UPSTREAM) {
    return GFNT_ERR_INVALID;
  }
  total = p->cp_byte[p->length];
  if (offset > total) {
    offset = total;
  }
  if (affinity == GFNT_AFFINITY_UPSTREAM) {
    for (size_t i = 0; i < l->line_count; i++) {
      if (l->lines[i].start < offset && offset <= text_end(p, &l->lines[i])) {
        li = i;
        break;
      }
    }
  }
  if (li == (size_t)-1) {
    for (size_t i = 0; i < l->line_count; i++) {
      if (l->lines[i].start <= offset && offset < l->lines[i].end) {
        li = i;
        break;
      }
    }
  }
  if (li == (size_t)-1) {
    li = l->line_count - 1;   // the end of the text
  }
  r = line_boxes(p, l, &l->lines[li], &boxes, &n);
  if (r != GFNT_OK) {
    return r;
  }
  caret.line = li;
  caret.horizontal = is_vertical(l);
  {
    GFNT_F26Dot6 along;   // in the boxes' frame

    along = is_flipped(l) ? l->height - l->lines[li].offset - l->lines[li].extent
                          : l->lines[li].offset;
    caret.length = caret.horizontal ? l->lines[li].box.width : l->lines[li].box.height;
    caret.x = l->lines[li].box.x;
    caret.y = l->lines[li].box.y;
    {
    const Box * after = NULL, * before = NULL;

    for (size_t i = 0; i < n; i++) {
      if (!after && boxes[i].lo <= offset && offset < boxes[i].hi) {
        after = &boxes[i];
      }
      if (!before && boxes[i].hi == offset) {
        before = &boxes[i];
      }
    }
    if (affinity == GFNT_AFFINITY_UPSTREAM && before) {
      along = before->rtl ? before->x0 : before->x1;
    }
    else if (after) {
      along = after->rtl ? after->x1 : after->x0;
    }
    else if (before) {
      along = before->rtl ? before->x0 : before->x1;
    }
    along = canon(l, along);
    if (caret.horizontal) {
      caret.y = along;
    }
    else {
      caret.x = along;
    }
    }
  }
  p->allocator->free_fn(p->allocator->ctx, boxes);
  *out = caret;
  return GFNT_OK;
}

GFNT_Result gfnt_layout_range_rects(const GFNT_Paragraph * p,
    const GFNT_Layout * l, size_t start, size_t end, GFNT_Rect * out,
    size_t capacity, size_t * count) {
  size_t total = 0;

  if (!valid(p, l) || !count) {
    return GFNT_ERR_INVALID;
  }
  for (size_t li = 0; end > start && li < l->line_count; li++) {
    const GFNT_Line * line = &l->lines[li];
    Box * boxes = NULL;
    size_t n = 0;
    bool open = false;
    GFNT_F26Dot6 base_ascent, base_descent;
    GFNT_Rect cur = {0, 0, 0, 0};   // x and width: the span along the line, in the boxes' frame
    GFNT_Result r;

    if (line->end <= start || line->start >= end) {
      continue;
    }
    base_line_metrics(p, line, &base_ascent, &base_descent);
    r = line_boxes(p, l, line, &boxes, &n);
    if (r != GFNT_OK) {
      return r;
    }
    for (size_t i = 0; i <= n; i++) {
      bool in = i < n && boxes[i].lo < end && boxes[i].hi > start;

      if (in && open && boxes[i].x0 == cur.x + cur.width) {
        cur.width = boxes[i].x1 - cur.x;   // along the line, in the boxes' frame for now
        continue;
      }
      if (open) {
        GFNT_Rect rect = cur;

        if (is_vertical(l)) {
          const GFNT_F26Dot6 cross_extent = base_ascent + base_descent;
          rect.x = line->baseline - cross_extent / 2;
          rect.y = is_flipped(l) ? l->height - (cur.x + cur.width) : cur.x;
          rect.width = cross_extent;
          rect.height = cur.width;
        }
        else {
          rect.y = line->baseline - base_ascent;
          rect.height = base_ascent + base_descent;
        }
        if (out && total < capacity) {
          out[total] = rect;
        }
        total++;
        open = false;
      }
      if (in) {
        cur.x = boxes[i].x0;
        cur.width = boxes[i].x1 - boxes[i].x0;
        open = true;
      }
    }
    p->allocator->free_fn(p->allocator->ctx, boxes);
  }
  *count = total;
  return GFNT_OK;
}
