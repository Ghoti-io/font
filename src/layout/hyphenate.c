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
 * Hyphenation points: soft hyphens and a caller-supplied dictionary.
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

static bool is_word_cp(uint32_t cp) {
  return (guni_general_category_mask(cp) & (GUNI_GC_MASK_L | GUNI_GC_MASK_M)) != 0;
}

/**
 * Mark the places a line may break with a hyphen added: after a soft hyphen (unless hyphens are
 * off, when a soft hyphen is no place at all), and where the hyphenator says inside a word.
 * Then set the width and the character of the hyphen for each span that has such a place.
 */
GFNT_Result gfnt_lay_mark_hyphens(GFNT_Paragraph * p, const char * text, const size_t * span_cp,
    GFNT_Error * error) {
  const GFNT_Allocator * a = p->allocator;
  const GFNT_Hyphenator * hy = &p->options.hyphenator;
  const bool automatic = p->options.hyphens == GFNT_HYPHENS_AUTO && hy->points
      && p->options.word_break != GFNT_WORD_BREAK_BREAK_ALL;
  size_t * unit_by_end = NULL;
  uint8_t * allowed = NULL;
  size_t allowed_cap = 0;
  bool * wanted = NULL;
  GFNT_Result r = GFNT_OK;
  bool any = false;
  size_t i;

  for (i = 0; i < p->unit_count; i++) {
    Unit * u = &p->units[i];

    if (p->cps[u->cp_end - 1] != 0x00AD) {
      continue;
    }
    if (p->options.hyphens == GFNT_HYPHENS_NONE) {
      u->opportunity = false;
    }
    else if (!u->hard) {
      // A soft hyphen is a place to break, and a hyphen is shown there, unless the character before
      // it is an ideograph: neither Chromium nor Pango shows one, and Chromium does not break there
      // either (Pango does).
      const bool ideograph = u->cp_end >= 2 && gfnt_lay_is_ideographic(p->cps[u->cp_end - 2]);

      u->opportunity = !ideograph || p->options.hyphens == GFNT_HYPHENS_MANUAL_PANGO;
      u->hyphen = !ideograph;
      any = any || u->hyphen;
    }
  }
  if (automatic && p->length > 1 && p->unit_count) {
    size_t c = 0;
    size_t sp = 0;

    unit_by_end = a->calloc_fn(a->ctx, p->length + 1, sizeof *unit_by_end);
    if (!unit_by_end) {
      return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "no memory for hyphenation");
    }
    for (i = 0; i <= p->length; i++) {
      unit_by_end[i] = (size_t)-1;
    }
    for (i = 0; i < p->unit_count; i++) {
      unit_by_end[p->units[i].cp_end] = i;
    }
    while (c < p->length && r == GFNT_OK) {
      size_t first;
      size_t limit;
      size_t bytes;
      bool soft = false;

      if (!is_word_cp(p->cps[c])) {
        c++;
        continue;
      }
      first = c;
      while (sp + 1 < p->span_count && span_cp[sp + 1] <= first) {
        sp++;
      }
      limit = sp + 1 < p->span_count ? span_cp[sp + 1] : p->length;
      while (c < limit && (is_word_cp(p->cps[c]) || p->cps[c] == 0x00AD)) {
        soft = soft || p->cps[c] == 0x00AD;
        c++;
      }
      if (c - first < 2 || soft) {
        continue;
      }
      bytes = p->cp_byte[c] - p->cp_byte[first];
      if (bytes + 1 > allowed_cap) {
        a->free_fn(a->ctx, allowed);
        allowed_cap = bytes + 1;
        allowed = a->calloc_fn(a->ctx, allowed_cap, 1);
        if (!allowed) {
          r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
              "no memory for hyphenation");
          break;
        }
      }
      else {
        memset(allowed, 0, bytes + 1);
      }
      r = hy->points(hy->ctx, text + p->cp_byte[first], bytes, p->styles[sp].language, allowed);
      if (r != GFNT_OK) {
        break;
      }
      for (i = first + 1; i < c; i++) {
        const size_t k = p->cp_byte[i] - p->cp_byte[first];
        const size_t unit = unit_by_end[i];

        if (p->cp_byte[i] > p->cp_byte[i - 1] && allowed[k]
            && unit != (size_t)-1 && !p->units[unit].hard
            && !p->units[unit].opportunity) {
          p->units[unit].opportunity = true;
          p->units[unit].hyphen = !gfnt_lay_is_ideographic(p->cps[i - 1]);
          any = any || p->units[unit].hyphen;
        }
      }
    }
  }
  a->free_fn(a->ctx, unit_by_end);
  a->free_fn(a->ctx, allowed);
  if (r != GFNT_OK || !any) {
    return r;
  }
  p->hyphen_width = a->calloc_fn(a->ctx, p->span_count, sizeof *p->hyphen_width);
  p->hyphen_cp = a->calloc_fn(a->ctx, p->span_count, sizeof *p->hyphen_cp);
  wanted = a->calloc_fn(a->ctx, p->span_count, sizeof *wanted);
  if (!p->hyphen_width || !p->hyphen_cp || !wanted) {
    a->free_fn(a->ctx, wanted);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE, "no memory for hyphenation");
  }
  for (i = 0; i < p->unit_count; i++) {
    if (p->units[i].hyphen) {
      wanted[p->runs[p->units[i].run].span] = true;
    }
  }
  for (i = 0; i < p->span_count && r == GFNT_OK; i++) {
    PGlyph g[ELL_GLYPHS];
    size_t count = 0;
    GFNT_F26Dot6 width = 0;
    PRun run;

    if (!wanted[i]) {
      continue;
    }
    p->hyphen_cp[i] = gfnt_lay_face_maps(p->styles[i].faces[0], 0x2010) ? 0x2010 : 0x2D;
    r = gfnt_lay_shape_string(p, i, &p->hyphen_cp[i], 1, g, &count, &width, &run, gfnt_lay_hyphen_orientation(p), error);
    p->hyphen_width[i] = width;
    if (count > p->hyphen_glyphs) {
      p->hyphen_glyphs = count;
    }
  }
  a->free_fn(a->ctx, wanted);
  return r;
}
