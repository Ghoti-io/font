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
 * Placing a line: slices in visual order, glyph positions and alignment.
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

/** Place the visual slices of a measured line, including glyph ink and justification. */
void gfnt_lay_place_line(PlacementState * state, GFNT_Line * line, size_t slice_count) {
  const GFNT_Paragraph * p = state->paragraph;

  line->runs = &state->runs[*state->run_used];
  line->run_count = slice_count;
  for (size_t v = 0; v < slice_count; v++) {
    const Slice * slice = &state->slices[state->order[v]];
    const PRun * pr = slice->pr;
    GFNT_LineRun * run = &state->runs[(*state->run_used)++];
    const size_t run_first = *state->glyph_used;

    run->face = pr->face;
    run->span = pr->span;
    run->face_index = pr->face_index;
    run->fallback = pr->face_index != 0;
    run->ellipsis = slice->ellipsis;
    run->hyphen = slice->hyphen;
    run->level = slice->level;
    run->orientation = (GFNT_Orientation)pr->orient;
    run->glyphs = &state->placed[*state->glyph_used];
    run->pos = state->cursor;
    for (size_t g = slice->g_lo; g < slice->g_hi; g++) {
      const PGlyph * glyphs = &slice->src[g];
      size_t glyph_count = 1;

      if (line->justified && !slice->hang && slice->src == p->glyphs
          && p->kcount && state->gk[g]) {
        if (state->gk[g] == KASHIDA_SKIP) continue;
        glyphs = &state->spool[state->kres[state->gk[g] - 1].first];
        glyph_count = state->kres[state->gk[g] - 1].count;
      }
      for (size_t q = 0; q < glyph_count; q++) {
        const PGlyph * pg = &glyphs[q];
        GFNT_PlacedGlyph * placed = &state->placed[(*state->glyph_used)++];
        GFNT_F26Dot6 advance = pg->tab && slice->src == p->glyphs
            ? state->tabadv[g] : pg->advance;
        GFNT_F26Dot6 before = 0, after = 0, origin, l, t, r, b;
        GFNT_F26Dot6 trim_before = 0, trim_here = 0;
        GFNT_F26Dot6 shift = 0;
        if (slice->src == p->glyphs) advance -= state->ruby_reclaim[g];
        if (slice->src == p->glyphs && glyphs == &slice->src[g]
            && pg->unit < p->unit_count && state->spacing_trim[pg->unit]) {
          const Unit * unit = &p->units[pg->unit];
          int64_t remaining = state->spacing_trim[pg->unit];

          /* Reduce the latest glyph advances first. A cluster can end in zero-
           * advance combining marks, so its whole trim cannot always be taken
           * from the final glyph. Later glyph offsets compensate for any earlier
           * advance reduction, preserving their shaped positions. */
          for (size_t q = unit->g_hi; q > unit->g_lo && remaining > 0;) {
            size_t candidate = --q;
            int64_t available = (int64_t)p->glyphs[candidate].advance
                - state->ruby_reclaim[candidate];
            int64_t reduction = available > 0
                ? (available < remaining ? available : remaining) : 0;
            if (candidate == g) trim_here = (GFNT_F26Dot6)reduction;
            else if (candidate < g) trim_before += (GFNT_F26Dot6)reduction;
            remaining -= reduction;
          }
          if (remaining > 0 || trim_here > advance) {
            state->failed = true;
            return;
          }
          advance -= trim_here;
        }
        if (slice->src == p->glyphs && glyphs == &slice->src[g]
            && pg->unit >= state->unit_start
            && pg->unit + 1 < state->unit_end) {
          GFNT_F26Dot6 gap = p->units[pg->unit].autospace_after;
          if (gap > 0 && advance > INT32_MAX - gap) {
            state->failed = true;
            return;
          }
          advance += gap;
        }
        if (slice->hang && (p->options.white_space == GFNT_WHITE_SPACE_NORMAL
                || p->options.white_space == GFNT_WHITE_SPACE_NOWRAP
                || p->options.white_space == GFNT_WHITE_SPACE_PRE_LINE)) {
          advance = 0;
        }
        if (advance < 0) {
          state->failed = true;
          return;
        }
        if (!gfnt_lay_inline_shift(p, pr, state->line_ascent, state->line_descent, &shift)) {
          state->failed = true;
          return;
        }

        if (line->justified && !slice->hang && slice->src == p->glyphs
            && glyphs == &slice->src[g]) {
          if (state->gjf[g] & 1) {
            before = state->extra + (state->expanded < (size_t)state->extra_rem ? 1 : 0);
            state->expanded++;
          }
          if (state->gjf[g] & 2) {
            after = state->extra + (state->expanded < (size_t)state->extra_rem ? 1 : 0);
            state->expanded++;
          }
        }
        origin = state->cursor + before + trim_before;
        placed->glyph = pg->glyph;
        placed->cluster = pg->cluster;
        placed->advance = before + advance + after;
        if (!state->vertical) {
          placed->pen_x = state->cursor;
          placed->pen_y = line->baseline - shift;
          placed->x = origin + pg->dx;
          placed->y = line->baseline - shift - pg->dy;
          l = placed->x + pg->ix0; t = placed->y - pg->iy1;
          r = placed->x + pg->ix1; b = placed->y - pg->iy0;
        }
        else if (pr->orient == GFNT_ORIENT_RUN_UPRIGHT) {
          placed->pen_x = shift;
          placed->pen_y = state->cursor;
          placed->x = shift + pg->dx;
          placed->y = origin - pg->dy;
          l = placed->x + pg->ix0; t = placed->y - pg->iy1;
          r = placed->x + pg->ix1; b = placed->y - pg->iy0;
        }
        else {
          placed->pen_x = -(pr->ascent - pr->descent) / 2 + shift;
          placed->pen_y = state->cursor;
          placed->x = placed->pen_x + pg->dy;
          placed->y = origin + pg->dx;
          l = placed->x + pg->iy0; t = placed->y + pg->ix0;
          r = placed->x + pg->iy1; b = placed->y + pg->ix1;
        }
        state->cursor += before + advance + after;
        if (pg->has_ink) {
          if (!state->ink_set) {
            state->ink_l = l; state->ink_t = t; state->ink_r = r; state->ink_b = b;
            state->ink_set = true;
          }
          else {
            if (l < state->ink_l) state->ink_l = l;
            if (t < state->ink_t) state->ink_t = t;
            if (r > state->ink_r) state->ink_r = r;
            if (b > state->ink_b) state->ink_b = b;
          }
        }
      }
    }
    run->glyph_count = *state->glyph_used - run_first;
    run->advance = state->cursor - run->pos;
    run->object = pr->object ? pr->object->user : NULL;
    run->start = slice->b_lo;
    run->end = slice->b_hi;
    if (slice->hang && !state->seen_text) state->lead += run->advance;
    if (!slice->hang) {
      state->seen_text = true;
      line->extent += run->advance;
    }
  }
  if (state->ink_set) {
    line->ink.x = state->ink_l;
    line->ink.y = state->ink_t;
    line->ink.width = state->ink_r - state->ink_l;
    line->ink.height = state->ink_b - state->ink_t;
  }
}

/** Apply paragraph alignment and convert a line from line-relative to physical coordinates. */
static void align_line(const GFNT_Paragraph * p, GFNT_Line * line,
    GFNT_F26Dot6 band_start, GFNT_F26Dot6 available, GFNT_F26Dot6 block_extent,
    bool para_rtl, bool vertical) {
  GFNT_F26Dot6 slack = available - line->extent;
  GFNT_F26Dot6 off;
  GFNT_Align align = p->options.align;
  bool last = line->end == p->cp_byte[p->length];
  size_t lo = 0, hi = p->length + 1;
  GFNT_F26Dot6 shift;
  GFNT_F26Dot6 across = 0;

  if (!last) {
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if (p->cp_byte[mid] < line->end) lo = mid + 1;
      else hi = mid;
    }
    last = lo > 0 && lo <= p->length && p->cp_byte[lo] == line->end
        && gfnt_lay_paragraph_hard_break(p, p->cps[lo - 1]);
  }
  if (last && p->options.align_last != GFNT_ALIGN_LAST_AUTO) {
    switch (p->options.align_last) {
      case GFNT_ALIGN_LAST_START: align = GFNT_ALIGN_START; break;
      case GFNT_ALIGN_LAST_END: align = GFNT_ALIGN_END; break;
      case GFNT_ALIGN_LAST_LEFT: align = GFNT_ALIGN_LEFT; break;
      case GFNT_ALIGN_LAST_RIGHT: align = GFNT_ALIGN_RIGHT; break;
      case GFNT_ALIGN_LAST_CENTER: align = GFNT_ALIGN_CENTER; break;
      case GFNT_ALIGN_LAST_JUSTIFY: align = GFNT_ALIGN_JUSTIFY; break;
      default: break;
    }
  }
  else if (last && align == GFNT_ALIGN_JUSTIFY) {
    align = GFNT_ALIGN_START;
  }
  if (align == GFNT_ALIGN_JUSTIFY) {
    align = GFNT_ALIGN_START;
  }
  if (align == GFNT_ALIGN_START) {
    align = para_rtl ? GFNT_ALIGN_RIGHT : GFNT_ALIGN_LEFT;
  }
  else if (align == GFNT_ALIGN_END) {
    align = para_rtl ? GFNT_ALIGN_LEFT : GFNT_ALIGN_RIGHT;
  }
  if (slack < 0) {
    off = para_rtl ? slack : 0;
  }
  else {
    off = align == GFNT_ALIGN_RIGHT ? slack
        : align == GFNT_ALIGN_CENTER ? slack / 2 : 0;
  }
  shift = off + line->offset;
  line->offset = band_start + off;
  if (vertical) {
    const GFNT_F26Dot6 before = line->box.x;

    line->box.x = p->mode == GFNT_WRITING_VERTICAL_LR
        ? before : block_extent - before - line->box.width;
    line->box.y = band_start;
    line->box.height = available;
    across = line->box.x + line->box.width / 2;
    line->baseline = across;
  }
  else {
    line->box.x = band_start;
    line->box.width = available;
  }
  if (line->ink.width || line->ink.height) {
    if (vertical) {
      line->ink.x += across;
      line->ink.y += shift;
    }
    else {
      line->ink.x += shift;
    }
  }
  for (size_t k = 0; k < line->run_count; k++) {
    GFNT_LineRun * run = (GFNT_LineRun *)&line->runs[k];

    run->pos += shift;
    for (size_t g = 0; g < run->glyph_count; g++) {
      GFNT_PlacedGlyph * glyph = (GFNT_PlacedGlyph *)&run->glyphs[g];

      if (vertical) {
        glyph->pen_x += across;
        glyph->x += across;
        glyph->pen_y += shift;
        glyph->y += shift;
      }
      else {
        glyph->pen_x += shift;
        glyph->x += shift;
      }
    }
  }
  if (p->mode == GFNT_WRITING_SIDEWAYS_LR) {
    const GFNT_F26Dot6 bw = block_extent, bh = band_start + available;

    line->box.x = bw - (line->box.x + line->box.width);
    line->offset = bh - (line->offset + line->extent);
    line->baseline = bw - line->baseline;
    if (line->ink.width || line->ink.height) {
      line->ink.x = bw - (line->ink.x + line->ink.width);
      line->ink.y = bh - (line->ink.y + line->ink.height);
    }
    for (size_t k = 0; k < line->run_count; k++) {
      GFNT_LineRun * run = (GFNT_LineRun *)&line->runs[k];

      run->pos = bh - (run->pos + run->advance);
      if (run->orientation == GFNT_ORIENT_RUN_CW) {
        run->orientation = GFNT_ORIENT_RUN_CCW;
      }
      for (size_t g = 0; g < run->glyph_count; g++) {
        GFNT_PlacedGlyph * glyph = (GFNT_PlacedGlyph *)&run->glyphs[g];

        glyph->pen_x = bw - glyph->pen_x;
        glyph->pen_y = bh - glyph->pen_y;
        glyph->x = bw - glyph->x;
        glyph->y = bh - glyph->y;
      }
    }
  }
}

/** Align every line and union the placed glyph ink boxes. */
GFNT_Rect gfnt_lay_align_lines(const GFNT_Paragraph * p, GFNT_Line * lines, size_t line_count,
    const GFNT_F26Dot6 * band_starts, const GFNT_F26Dot6 * band_widths,
    GFNT_F26Dot6 default_width, GFNT_F26Dot6 widest, GFNT_F26Dot6 block_extent,
    bool para_rtl, bool vertical) {
  GFNT_Rect ink = {0, 0, 0, 0};
  bool ink_any = false;

  for (size_t li = 0; li < line_count; li++) {
    GFNT_Line * line = &lines[li];

    GFNT_F26Dot6 available = band_widths[li];
    if (default_width == GFNT_LAYOUT_UNBOUNDED) available = widest;
    align_line(p, line, band_starts[li], available, block_extent, para_rtl, vertical);
    if (line->ink.width || line->ink.height) {
      if (ink_any) {
        GFNT_F26Dot6 r1 = ink.x + ink.width, b1 = ink.y + ink.height;
        GFNT_F26Dot6 r2 = line->ink.x + line->ink.width;
        GFNT_F26Dot6 b2 = line->ink.y + line->ink.height;

        if (line->ink.x < ink.x) ink.x = line->ink.x;
        if (line->ink.y < ink.y) ink.y = line->ink.y;
        ink.width = (r2 > r1 ? r2 : r1) - ink.x;
        ink.height = (b2 > b1 ? b2 : b1) - ink.y;
      }
      else {
        ink = line->ink;
        ink_any = true;
      }
    }
  }
  return ink;
}
