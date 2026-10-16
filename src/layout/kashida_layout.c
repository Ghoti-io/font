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
 * Kashida in a laid-out paragraph: reshaping a word with tatweels and fitting a stroke.
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

/** The glyphs of a shaping that stand for a code point, with the code point (or SIZE_MAX). */
static size_t shaped_cp(const PGlyph * g, size_t first_cp, size_t at_tw, size_t n_tw) {
  size_t c = g->cluster;

  if (at_tw == SIZE_MAX || c < at_tw) {
    return first_cp + c;
  }
  if (c < at_tw + n_tw) {
    return SIZE_MAX;           // a tatweel
  }
  return first_cp + c - n_tw;
}

/**
 * What stretching the word at the place in front of unit @p u with @p n_tw tatweels would be: the
 * word is shaped (the units around the place, a window of the word) with the tatweels in it and
 * without. Without, it must come out as the paragraph has it; with, whatever the font makes of
 * the letters and the tatweels (Amiri draws a jeem before an alef differently from one before a
 * tatweel, and a run of tatweels as one longer glyph) replaces the paragraph's glyphs for those
 * units, which the stroke, when drawn, takes the place of. @p out receives the new glyphs.
 * @p ok is false where there is no such place: a unit of another run or face, a window the probe
 * cannot shape as the paragraph was, a font with no tatweel, or no growth.
 */
static GFNT_Result kashida_reshape(const GFNT_Paragraph * p, const KPoint * pt, size_t n_tw,
    PGlyph * out, size_t out_cap, KReshape * k, bool * ok, GFNT_Error * error) {
  PGlyph gw[KASHIDA_PROBE], gc[KASHIDA_PROBE];
  uint32_t s[KASHIDA_PROBE];
  size_t ocp[KASHIDA_PROBE];
  size_t nw = 0, nc = 0, count = 0, at_tw, first_cp, last_cp, cp_lo, cp_hi, g_lo, g_hi, m = 0;
  size_t u = pt->unit;
  const Unit * a = &p->units[u - 1], * b = &p->units[u];
  const PRun * pr = &p->runs[b->run];
  const size_t before = b->cp_start;
  GFNT_F26Dot6 total, grown = 0, was = 0, tws = 0;
  GFNT_Result r;
  PRun run;
  bool rtl;

  *ok = false;
  if (a->cp_end != before || a->run != b->run || pr->face_index != 0
      || pr->orient != GFNT_ORIENT_RUN_HORIZONTAL) {
    return GFNT_OK;
  }
  rtl = (pr->level & 1) != 0;
  g_lo = SIZE_MAX;
  g_hi = 0;
  for (size_t v = pt->lo; v <= pt->hi; v++) {
    const Unit * x = &p->units[v];

    if (x->run != b->run) {
      return GFNT_OK;
    }
    if (x->g_lo < g_lo) {
      g_lo = x->g_lo;
    }
    if (x->g_hi > g_hi) {
      g_hi = x->g_hi;
    }
  }
  first_cp = p->units[pt->win_lo].cp_start;
  last_cp = p->units[pt->win_hi].cp_end;
  cp_lo = p->units[pt->lo].cp_start;
  cp_hi = p->units[pt->hi].cp_end;
  if (last_cp - first_cp + n_tw > KASHIDA_PROBE || g_hi <= g_lo) {
    return GFNT_OK;
  }
  for (size_t i = first_cp; i < last_cp; i++) {
    if (i == before) {
      for (size_t t = 0; t < n_tw; t++) {
        s[count++] = 0x0640;
      }
    }
    s[count++] = p->cps[i];
  }
  at_tw = before - first_cp;
  r = gfnt_lay_shape_string_dir(p, pr->span, s, count, gw, &nw, &total, &run,
      GFNT_ORIENT_RUN_HORIZONTAL, rtl ? 1 : 0, KASHIDA_PROBE, error);
  if (r != GFNT_OK) {
    return r;
  }
  // The same without the tatweels.
  memmove(&s[at_tw], &s[at_tw + n_tw], (count - at_tw - n_tw) * sizeof s[0]);
  r = gfnt_lay_shape_string_dir(p, pr->span, s, count - n_tw, gc, &nc, &total, &run,
      GFNT_ORIENT_RUN_HORIZONTAL, rtl ? 1 : 0, KASHIDA_PROBE, error);
  if (r != GFNT_OK) {
    return r;
  }
  // Without them, the paragraph's own glyphs for these units.
  {
    size_t real = g_lo;

    for (size_t i = 0; i < nc; i++) {
      const size_t cp = shaped_cp(&gc[i], first_cp, SIZE_MAX, 0);

      if (cp < cp_lo || cp >= cp_hi) {
        continue;
      }
      if (real >= g_hi || gc[i].glyph != p->glyphs[real].glyph || gc[i].dx != p->glyphs[real].dx
          || gc[i].dy != p->glyphs[real].dy) {
        return GFNT_OK;
      }
      real++;
    }
    if (real != g_hi) {
      return GFNT_OK;
    }
  }
  // With them: the glyphs of these units and of the tatweels.
  for (size_t i = 0; i < nw; i++) {
    const size_t cp = shaped_cp(&gw[i], first_cp, at_tw, n_tw);

    if (cp != SIZE_MAX && (cp < cp_lo || cp >= cp_hi)) {
      continue;
    }
    if (m == out_cap) {
      return GFNT_OK;
    }
    out[m] = gw[i];
    ocp[m] = cp;
    if (cp == SIZE_MAX) {
      if (gw[i].glyph == 0 || gw[i].advance <= 0) {
        return GFNT_OK;          // the face has no tatweel
      }
      out[m].cluster = (uint32_t)p->cp_byte[a->cp_start];   // the stroke belongs to the letter before
      tws += gw[i].advance;
    }
    else {
      out[m].cluster = (uint32_t)p->cp_byte[cp];
    }
    m++;
  }
  if (m == 0 || m > (g_hi - g_lo) + n_tw + 4) {
    return GFNT_OK;
  }
  // Spacing was added to the paragraph's glyphs, one to a unit: do the same here.
  {
    const GFNT_F26Dot6 spacing = p->styles[pr->span].letter_spacing;
    const GFNT_F26Dot6 word_spacing = p->styles[pr->span].word_spacing;

    if (spacing) {
      for (size_t v = pt->lo; v <= pt->hi; v++) {
        const Unit * x = &p->units[v];
        size_t pick = SIZE_MAX;

        for (size_t i = 0; i < m; i++) {
          if (ocp[i] != SIZE_MAX && ocp[i] >= x->cp_start && ocp[i] < x->cp_end
              && (pick == SIZE_MAX || !rtl)) {
            pick = i;
            if (rtl) {
              break;
            }
          }
        }
        if (pick != SIZE_MAX) {
          const int64_t advance = (int64_t)out[pick].advance + spacing;
          if (advance < INT32_MIN || advance > INT32_MAX) {
            return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, out[pick].glyph,
                "letter spacing overflows a reshaped glyph advance");
          }
          out[pick].advance = (GFNT_F26Dot6)advance;
        }
      }
    }
    if (word_spacing) {
      for (size_t v = pt->lo; v <= pt->hi; v++) {
        const Unit * x = &p->units[v];

        if (x->cp_end == x->cp_start + 1 && p->cps[x->cp_start] == 0x20) {
          for (size_t i = 0; i < m; i++) {
            if (ocp[i] == x->cp_start) {
              const int64_t advance = (int64_t)out[i].advance + word_spacing;
              if (advance < INT32_MIN || advance > INT32_MAX) {
                return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, out[i].glyph,
                    "word spacing overflows a reshaped glyph advance");
              }
              out[i].advance = (GFNT_F26Dot6)advance;
              break;
            }
          }
        }
      }
    }
  }
  for (size_t i = 0; i < m; i++) {
    grown += out[i].advance;
  }
  for (size_t g = g_lo; g < g_hi; g++) {
    was += p->glyphs[g].advance;
  }
  if (grown <= was) {
    return GFNT_OK;
  }
  k->g_lo = g_lo;
  k->g_hi = g_hi;
  k->count = m;
  k->growth = grown - was;
  // What one tatweel is worth: its own glyph's advance, or if the font merged it into a letter's
  // glyph, what it added.
  k->adv1 = (tws > 0 ? tws : k->growth) / (GFNT_F26Dot6)n_tw;
  *ok = k->adv1 > 0;
  return GFNT_OK;
}

/** What the chooser asks of a paragraph: whether a tatweel can go in front of a code point. */
typedef struct {
  const GFNT_Paragraph * p;
  const size_t * unit_at;      // per code point: the unit that starts there, or SIZE_MAX
  size_t word, word_end;       // the word: the chooser counts from its start
  KPoint pt;                   // the last place allowed
  GFNT_Result r;
  GFNT_Error * error;
} KashidaProbe;

/**
 * Whether a tatweel can go in front of the code point @p word_before of the word: see
 * kashida_reshape(), here with one tatweel. The place also fixes the window: the units of the
 * word, or eight either side of the place. Where the word goes on past it, the two units at the
 * window's ends are left out of what is replaced, for the probe cannot know what shaped them.
 */
static bool kashida_allowed(void * vctx, size_t word_before) {
  KashidaProbe * c = vctx;
  const size_t before = c->word + word_before;
  const GFNT_Paragraph * p = c->p;
  PGlyph out[KASHIDA_PROBE];
  KReshape info;
  KPoint pt;
  size_t u, word_u0, word_u1;
  bool ok;

  if (c->r != GFNT_OK || c->unit_at[before] == SIZE_MAX || c->unit_at[before] == 0
      || c->unit_at[c->word] == SIZE_MAX) {
    return false;
  }
  u = c->unit_at[before];
  if (p->units[u].cp_start != before || p->units[u].g_hi <= p->units[u].g_lo
      || p->units[u - 1].g_hi <= p->units[u - 1].g_lo) {
    return false;
  }
  word_u0 = c->unit_at[c->word];
  word_u1 = u;
  while (word_u1 + 1 < p->unit_count && p->units[word_u1 + 1].cp_start < c->word_end) {
    word_u1++;
  }
  pt.unit = u;
  pt.win_lo = u > word_u0 + KASHIDA_REACH ? u - KASHIDA_REACH : word_u0;
  pt.win_hi = u + KASHIDA_REACH < word_u1 ? u + KASHIDA_REACH : word_u1;
  pt.lo = pt.win_lo > word_u0 ? pt.win_lo + 2 : pt.win_lo;
  pt.hi = pt.win_hi < word_u1 ? pt.win_hi - 2 : pt.win_hi;
  pt.adv1 = 0;
  if (pt.lo > u - 1 || pt.hi < u) {
    return false;
  }
  c->r = kashida_reshape(p, &pt, 1, out, KASHIDA_PROBE, &info, &ok, c->error);
  if (c->r != GFNT_OK || !ok) {
    return false;
  }
  pt.adv1 = info.adv1;
  c->pt = pt;
  return true;
}

/**
 * The most tatweels that fit in @p share more than the glyphs they replace take, found by
 * shaping the point with a few counts: the best is left in @p out and @p k. @p fits is false if
 * not even one fits.
 */
GFNT_Result gfnt_lay_kashida_fit(const GFNT_Paragraph * p, const KPoint * pt, GFNT_F26Dot6 share,
    PGlyph * out, KReshape * k, bool * fits, GFNT_Error * error) {
  PGlyph trial[KASHIDA_PROBE];
  KReshape one, two, tk;
  size_t n, best = 0;
  bool ok;
  GFNT_Result r;

  *fits = false;
  // Two tatweels and three give how much each adds (the letters beside them change once, not
  // with every one), which says how many to try first.
  r = kashida_reshape(p, pt, 1, trial, KASHIDA_PROBE, &one, &ok, error);
  if (r != GFNT_OK || !ok) {
    return r;
  }
  if (one.growth <= share) {
    best = 1;
    *k = one;
    for (size_t i = 0; i < one.count; i++) {
      out[i] = trial[i];
    }
  }
  r = kashida_reshape(p, pt, 2, trial, KASHIDA_PROBE, &two, &ok, error);
  if (r != GFNT_OK) {
    return r;
  }
  if (ok && two.growth > one.growth) {
    const GFNT_F26Dot6 slope = two.growth - one.growth;
    const GFNT_F26Dot6 base = one.growth - slope;     // what the letters change by

    n = share > base ? (size_t)((share - base) / slope) : 1;
  }
  else {
    n = (size_t)(share / pt->adv1);
  }
  if (n < 1) {
    n = 1;
  }
  if (n > KASHIDA_COPIES_MAX) {
    n = KASHIDA_COPIES_MAX;
  }
  for (int step = 0; step < 8; step++) {
    r = kashida_reshape(p, pt, n, trial, KASHIDA_PROBE, &tk, &ok, error);
    if (r != GFNT_OK) {
      return r;
    }
    if (ok && tk.growth <= share) {
      // It fits: keep it, and see whether one more does.
      if (n > best) {
        best = n;
        *k = tk;
        for (size_t i = 0; i < tk.count; i++) {
          out[i] = trial[i];
        }
      }
      if (n >= KASHIDA_COPIES_MAX) {
        break;
      }
      n++;
      continue;
    }
    if (best >= n || n == 1) {
      break;                     // it does not, and a shorter one is already the best
    }
    n--;
  }
  *fits = best != 0;
  return GFNT_OK;
}

/**
 * For ::GFNT_JUSTIFY_KASHIDA, choose the place of every word that has one. Horizontal text only.
 */
GFNT_Result gfnt_lay_find_kashida(GFNT_Paragraph * p, GFNT_Error * error) {
  const GFNT_Allocator * a = p->allocator;
  size_t * unit_at;
  GFNT_Result r = GFNT_OK;
  KashidaProbe probe;
  size_t i;

  if (p->options.justify != GFNT_JUSTIFY_KASHIDA || p->vertical || p->length < 2) {
    return GFNT_OK;
  }
  unit_at = a->calloc_fn(a->ctx, p->length + 1, sizeof *unit_at);
  p->kpoints = a->calloc_fn(a->ctx, p->length / 2 + 1, sizeof *p->kpoints);
  if (!unit_at || !p->kpoints) {
    a->free_fn(a->ctx, unit_at);
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the layout");
  }
  for (i = 0; i <= p->length; i++) {
    unit_at[i] = SIZE_MAX;
  }
  for (i = 0; i < p->unit_count; i++) {
    unit_at[p->units[i].cp_start] = i;
  }
  memset(&probe, 0, sizeof probe);
  probe.p = p;
  probe.unit_at = unit_at;
  probe.r = GFNT_OK;
  probe.error = error;
  for (i = 0; i < p->length && probe.r == GFNT_OK;) {
    size_t end = i;
    size_t before;

    if (gfnt_lay_hangs(p->cps[i]) || p->cps[i] == 0x09) {
      i++;
      continue;
    }
    while (end < p->length && !gfnt_lay_hangs(p->cps[end]) && p->cps[end] != 0x09) {
      end++;
    }
    probe.word = i;
    probe.word_end = end;
    if (gfnt_kashida_point(&p->cps[i], end - i, &before, NULL, kashida_allowed, &probe)
        && probe.r == GFNT_OK) {
      p->kpoints[p->kcount] = probe.pt;
      if (!p->kcount || probe.pt.adv1 < p->kmin_adv) {
        p->kmin_adv = probe.pt.adv1;
      }
      p->units[probe.pt.unit - 1].kashida = (uint32_t)++p->kcount;
    }
    i = end;
  }
  r = probe.r;
  a->free_fn(a->ctx, unit_at);
  return r;
}
