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
 * Justification: marking opportunities and spreading a line's slack.
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

/**
 * The justification opportunities of the units [ua, hang_from) of a line, as flags on the glyphs
 * that take the space (bit 0: before the glyph, bit 1: after it, in the order the glyphs are
 * placed). Returns how many there are. Only units with glyphs can hold one.
 */
static size_t mark_justification(const GFNT_Paragraph * p, size_t ua, size_t hang_from,
    uint8_t * gjf) {
  const GFNT_Justify mode = p->options.justify;
  size_t n = 0;

  if (mode == GFNT_JUSTIFY_NONE) {
    return 0;
  }
  for (size_t j = ua; j < hang_from; j++) {
    const Unit * u = &p->units[j];
    const size_t first = u->g_lo, last = u->g_hi - 1;
    const bool rtl = (p->runs[u->run].level & 1) != 0;
    const bool more = j + 1 < hang_from;

    if (u->g_hi <= u->g_lo || u->tab) {
      continue;
    }
    if (mode == GFNT_JUSTIFY_INTER_CHARACTER) {
      if (more) {
        // After the unit in the text: its far edge as the glyphs are placed.
        gjf[rtl ? first : last] |= rtl ? 1 : 2;
        n++;
      }
      continue;
    }
    if (u->space) {
      gjf[last] |= 2;
      n++;
      continue;
    }
    if (mode == GFNT_JUSTIFY_AUTO && more) {
      const Unit * v = &p->units[j + 1];

      if (gfnt_lay_is_ideographic(p->cps[u->cp_start])) {
        // After an ideograph, whatever follows (a space included).
        gjf[rtl ? first : last] |= rtl ? 1 : 2;
        n++;
      }
      else if (!u->object && !v->space && !v->tab && v->g_hi > v->g_lo
          && gfnt_lay_is_ideographic(p->cps[v->cp_start])) {
        // Before an ideograph that follows something else but a space.
        const bool vrtl = (p->runs[v->run].level & 1) != 0;

        gjf[vrtl ? v->g_hi - 1 : v->g_lo] |= vrtl ? 2 : 1;
        n++;
      }
    }
  }
  return n;
}

/** Distribute line slack through spaces and any eligible Arabic joining strokes. */
GFNT_Result gfnt_lay_justify_line(JustificationState * state, GFNT_Line * line,
    size_t ua, size_t ub, size_t hang_from, GFNT_F26Dot6 natural, GFNT_F26Dot6 width,
    size_t li, size_t total_lines, bool bounded, bool truncated,
    GFNT_F26Dot6 * extra, GFNT_F26Dot6 * extra_rem, GFNT_Error * error) {
  const GFNT_Paragraph * p = state->paragraph;
  const bool paragraph_last = li + 1 == total_lines;
  const bool hard_last = ub > ua && p->units[ub - 1].hard;
  const bool align_last = paragraph_last || hard_last;
  const bool last_justify = p->options.align_last == GFNT_ALIGN_LAST_JUSTIFY
      || (p->options.align_last == GFNT_ALIGN_LAST_AUTO && p->options.justify_last);

  if ((p->options.align == GFNT_ALIGN_JUSTIFY
          || (align_last && p->options.align_last == GFNT_ALIGN_LAST_JUSTIFY))
      && bounded && !truncated
      && ub > ua && ((align_last && last_justify)
          || (!align_last && !hard_last))) {
    const size_t opportunities = mark_justification(p, ua, hang_from, state->gjf);
    GFNT_F26Dot6 slack = width - natural;
    size_t strokes = 0;

    if (p->kcount && slack > 0) {
      for (size_t j = ua; j + 1 < hang_from; j++) {
        const Unit * unit = &p->units[j];

        if (unit->kashida && p->kpoints[unit->kashida - 1].lo >= ua
            && p->kpoints[unit->kashida - 1].hi < hang_from) {
          strokes++;
        }
      }
    }
    if ((opportunities || strokes) && slack > 0) {
      GFNT_F26Dot6 spaced = slack;

      if (strokes) {
        GFNT_F26Dot6 want = opportunities
            ? (GFNT_F26Dot6)((int64_t)slack * (0x10000 - p->options.space_share) / 0x10000)
            : slack;
        GFNT_F26Dot6 each = want / (GFNT_F26Dot6)strokes;
        GFNT_F26Dot6 rem = want % (GFNT_F26Dot6)strokes;
        GFNT_F26Dot6 carry = 0;

        for (size_t j = ua; j + 1 < hang_from; j++) {
          const Unit * unit = &p->units[j];
          const KPoint * point;
          GFNT_F26Dot6 share;
          PGlyph fit_glyphs[KASHIDA_PROBE];
          KReshape fit;
          bool fits;
          GFNT_Result r;

          if (!unit->kashida) continue;
          point = &p->kpoints[unit->kashida - 1];
          if (point->lo < ua || point->hi >= hang_from) continue;
          share = each + (rem > 0 ? 1 : 0) + carry;
          if (rem > 0) rem--;
          r = gfnt_lay_kashida_fit(p, point, share, fit_glyphs, &fit, &fits, error);
          if (r != GFNT_OK) return r;
          if (!fits) {
            carry = share;
            continue;
          }
          carry = share - fit.growth;
          if (*state->spool_len + fit.count > *state->spool_cap) {
            size_t cap2 = *state->spool_cap ? *state->spool_cap * 2 : 256;
            PGlyph * grown;

            while (cap2 < *state->spool_len + fit.count) cap2 *= 2;
            grown = state->allocator->realloc_fn(state->allocator->ctx, *state->spool,
                cap2 * sizeof *grown);
            if (!grown) {
              return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
                  "no memory for the layout");
            }
            *state->spool = grown;
            *state->spool_cap = cap2;
          }
          fit.first = *state->spool_len;
          for (size_t q = 0; q < fit.count; q++) {
            (*state->spool)[(*state->spool_len)++] = fit_glyphs[q];
          }
          state->kres[unit->kashida - 1] = fit;
          state->gk[fit.g_lo] = unit->kashida;
          for (size_t g = fit.g_lo + 1; g < fit.g_hi; g++) state->gk[g] = KASHIDA_SKIP;
          spaced -= fit.growth;
        }
      }
      if (opportunities) {
        *extra = spaced / (GFNT_F26Dot6)opportunities;
        *extra_rem = spaced % (GFNT_F26Dot6)opportunities;
      }
      line->justified = true;
    }
    else {
      for (size_t j = ua; j < hang_from; j++) {
        for (size_t g = p->units[j].g_lo; g < p->units[j].g_hi; g++) state->gjf[g] = 0;
      }
    }
  }
  return GFNT_OK;
}
