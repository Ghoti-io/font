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
 * Paragraph layout: itemise and shape once, break and place per width.
 *
 * The shaped text is kept as *units*: the glyphs of one cluster, with the
 * code points they stand for, in logical order. A line is a range of units, so
 * a break can never fall inside a ligature, and a re-layout at a new width
 * touches no shaper. Each line's units are cut into slices of one face and one
 * level, put in visual order by UAX #9 rule L2, and placed.
 */

#include <ghoti.io/font/layout.h>
#include <ghoti.io/font/cmap.h>
#include <ghoti.io/font/glyph.h>
#include <ghoti.io/font/metrics.h>
#include <ghoti.io/font/outline.h>
#include <ghoti.io/unicode/bidi.h>
#include <ghoti.io/unicode/break.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/script.h>
#include <ghoti.io/unicode/utf.h>
#include <stdlib.h>
#include <string.h>
#include "layout_internal.h"

#define ALLOC(p, n) (allocator->calloc_fn(allocator->ctx, (n) ? (n) : 1, sizeof *(p)))

static bool is_newline(uint32_t cp) {
  return cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x85
      || cp == 0x2028 || cp == 0x2029;
}

static bool hangs(uint32_t cp) {
  if (cp == 0x20 || is_newline(cp)) {
    return true;
  }
  if (cp == 0xA0 || cp == 0x202F || cp == 0x2007) {
    return false;
  }
  return guni_general_category(cp) == GUNI_GC_ZS;
}

#define PROVIDER_MAX 8
#define ELL_GLYPHS 16

static GFNT_Result shape_string(const GFNT_Paragraph * p, size_t span,
    const uint32_t * text, size_t text_count, PGlyph * glyphs, size_t * count,
    GFNT_F26Dot6 * total, PRun * run, GFNT_Error * error);

/** The width of a unit that starts @p pos from the line's start: a tab goes to the next stop. */
static GFNT_F26Dot6 eff_width(const GFNT_Paragraph * p, const Unit * u,
    GFNT_F26Dot6 pos) {
  if (!u->tab || p->tab_width <= 0) {
    return u->width;
  }
  return p->tab_width - pos % p->tab_width;
}

static GFNT_F26Dot6 scale_v(GFNT_F26Dot6 v, GFNT_F16Dot16 scale) {
  int64_t t = (int64_t)v * scale;

  return (GFNT_F26Dot6)((t >= 0 ? t + 32768 : t - 32768) / 65536);
}

/** The outline box of a glyph in pixels, read once. Only an OOM is an error. */
static GFNT_Result glyph_ink(PGlyph * pg, const GFNT_Face * face,
    const GFNT_TextStyle * style, GFNT_F16Dot16 scale,
    const GFNT_Allocator * allocator) {
  GFNT_Outline * outline = NULL;
  GFNT_Box box;
  GFNT_Result r;

  pg->has_ink = false;
  r = gfnt_face_glyph_outline(face, pg->glyph, style->variation, allocator,
      &outline, NULL);
  if (r == GFNT_ERR_OOM) {
    return r;
  }
  if (r != GFNT_OK) {
    return GFNT_OK;
  }
  if (gfnt_outline_bounds(outline, &box) == GFNT_OK && !gfnt_box_is_empty(&box)) {
    pg->has_ink = true;
    pg->ix0 = scale_v(box.x_min, scale);
    pg->iy0 = scale_v(box.y_min, scale);
    pg->ix1 = scale_v(box.x_max, scale);
    pg->iy1 = scale_v(box.y_max, scale);
  }
  gfnt_outline_destroy(outline);
  return GFNT_OK;
}

static bool provider_skips(uint32_t cp) {
  GUNI_GeneralCategory gc = guni_general_category(cp);

  return gc == GUNI_GC_CC || gc == GUNI_GC_CF || gc == GUNI_GC_ZS;
}

static bool face_maps(const GFNT_Face * face, uint32_t cp) {
  uint32_t g = 0;

  return gfnt_face_glyph_for_codepoint(face, cp, &g, NULL) == GFNT_OK && g != 0;
}

static void scale_metrics(const GFNT_Face * face, const GFNT_TextStyle * style,
    PRun * run) {
  GFNT_LineMetrics m;
  uint16_t upem = 0;
  GFNT_F16Dot16 scale;

  if (gfnt_face_units_per_em(face, &upem, NULL) != GFNT_OK || upem == 0
      || gfnt_face_line_metrics(face, style->line_metrics, style->variation, &m,
             NULL) != GFNT_OK) {
    return;
  }
  scale = (GFNT_F16Dot16)(((int64_t)style->size * 1024 + upem / 2) / upem);
  run->ascent = gfnt_units_to_pixels(m.ascent, scale);
  run->descent = gfnt_units_to_pixels(-m.descent, scale);
  run->gap = gfnt_units_to_pixels(m.line_gap, scale);
}

/** The features of a span, cut to [from, to) of the whole text and rebased. */
static GFNT_ShapeFeature * cut_features(const GFNT_Allocator * allocator,
    const GFNT_TextStyle * style, size_t span_start, size_t from, size_t to,
    size_t * count) {
  GFNT_ShapeFeature * out;
  size_t n = 0;

  *count = 0;
  if (!style->feature_count) {
    return NULL;
  }
  out = ALLOC(out, style->feature_count);
  if (!out) {
    return NULL;
  }
  for (size_t i = 0; i < style->feature_count; i++) {
    GFNT_ShapeFeature f = style->features[i];
    size_t s = span_start + f.start;
    size_t e = f.end == GFNT_SHAPE_END ? to : span_start + f.end;

    if (s < from) {
      s = from;
    }
    if (e > to) {
      e = to;
    }
    if (s >= e) {
      continue;
    }
    f.start = s - from;
    f.end = e - from;
    out[n++] = f;
  }
  *count = n;
  return out;
}

void gfnt_paragraph_free(GFNT_Paragraph * p) {
  if (!p) {
    return;
  }
  const GFNT_Allocator * allocator = p->allocator;

  allocator->free_fn(allocator->ctx, p->cp_byte);
  allocator->free_fn(allocator->ctx, p->cps);
  allocator->free_fn(allocator->ctx, p->styles);
  allocator->free_fn(allocator->ctx, p->runs);
  allocator->free_fn(allocator->ctx, p->glyphs);
  allocator->free_fn(allocator->ctx, p->units);
  allocator->free_fn(allocator->ctx, p);
}

/** Append the units of one face run, in logical order. */
static GFNT_Result add_units(GFNT_Paragraph * p, const GFNT_FaceRun * fr,
    size_t item_start, bool rtl, size_t run, size_t glyph_base,
    const uint32_t * cps, GFNT_Error * error) {
  size_t first = item_start + fr->start;
  size_t last = first + fr->length;
  size_t count = fr->run.count;
  size_t unit_first = p->unit_count;

  if (count == 0) {
    Unit * u = &p->units[p->unit_count++];

    memset(u, 0, sizeof *u);
    u->cp_start = first;
    u->cp_end = last;
    u->run = run;
    u->g_lo = u->g_hi = glyph_base;
  }
  for (size_t k = 0; k < count; k++) {
    size_t g = rtl ? count - 1 - k : k;
    size_t cluster = item_start + fr->run.glyphs[g].cluster;
    Unit * prev = p->unit_count > unit_first ? &p->units[p->unit_count - 1] : NULL;

    if (cluster < first) {
      cluster = first;
    }
    if (prev && cluster <= prev->cp_start) {
      // The same cluster again, or one that runs backwards: the same unit.
      if (rtl) {
        prev->g_lo = glyph_base + g;
      }
      else {
        prev->g_hi = glyph_base + g + 1;
      }
      continue;
    }
    if (prev) {
      prev->cp_end = cluster;
    }
    Unit * u = &p->units[p->unit_count++];

    memset(u, 0, sizeof *u);
    u->cp_start = prev ? cluster : first;
    u->run = run;
    u->g_lo = glyph_base + g;
    u->g_hi = glyph_base + g + 1;
  }
  if (p->unit_count > unit_first) {
    p->units[p->unit_count - 1].cp_end = last;
  }
  for (size_t i = unit_first; i < p->unit_count; i++) {
    Unit * u = &p->units[i];
    bool all_space = u->cp_end > u->cp_start;

    GFNT_F26Dot6 spacing = p->styles[p->runs[run].span].letter_spacing;

    if (spacing && u->g_hi > u->g_lo) {
      p->glyphs[rtl ? u->g_lo : u->g_hi - 1].advance += spacing;
    }
    if (u->cp_end - u->cp_start == 1 && cps[u->cp_start] == 0x09
        && u->g_hi > u->g_lo) {
      u->tab = true;
      p->glyphs[u->g_lo].tab = true;
      p->tab_width = -1;   // to be resolved once the text is shaped
    }
    u->width = 0;
    for (size_t g = u->g_lo; g < u->g_hi; g++) {
      u->width += p->glyphs[g].advance;
    }
    for (size_t c = u->cp_start; c < u->cp_end; c++) {
      all_space = all_space && hangs(cps[c]);
    }
    u->space = all_space;
    if (all_space && u->g_hi > u->g_lo) {
      p->glyphs[u->g_lo].expand = true;
    }
  }
  (void)error;
  return GFNT_OK;
}

GFNT_Result gfnt_paragraph_create(const char * text, size_t length,
    const GFNT_TextSpan * spans, size_t span_count,
    const GFNT_ParagraphOptions * options, const GFNT_Allocator * allocator,
    GFNT_Paragraph ** out, GFNT_Error * error) {
  static const GFNT_ParagraphOptions zero;
  GFNT_Paragraph * p = NULL;
  uint32_t * cps = NULL;
  uint8_t * levels = NULL;
  size_t * span_cp = NULL;      // span_count + 1 code point starts
  bool * edge = NULL;           // item boundaries
  bool * opp = NULL;
  size_t * breaks = NULL;
  GUNI_ScriptItem * scripts = NULL;
  size_t n = 0;
  bool cps_owned = false;   // the paragraph holds cps, and frees it
  size_t total_runs = 0;
  GFNT_Result r = GFNT_OK;

  gfnt_error_clear(error);
  if ((!text && length) || !spans || !span_count || !out) {
    return GFNT_ERR_INVALID;
  }
  if (!options) {
    options = &zero;
  }
  if (!allocator) {
    allocator = gfnt_allocator_default();
  }
  if ((unsigned)options->direction > GFNT_BIDI_AUTO
      || (unsigned)options->align > GFNT_ALIGN_JUSTIFY
      || (unsigned)options->line_break > GFNT_LINE_BREAK_ANYWHERE) {
    return GFNT_ERR_INVALID;
  }
  {
    size_t sum = 0;

    for (size_t i = 0; i < span_count; i++) {
      const GFNT_TextStyle * s = &spans[i].style;

      if (!s->faces || !s->face_count || s->size <= 0) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "a span has no face or no size");
      }
      for (size_t f = 0; f < s->face_count; f++) {
        if (!s->faces[f]) {
          return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
              "a span's face list holds a NULL");
        }
      }
      if (spans[i].length > length - sum) {
        sum = length + 1;
        break;
      }
      sum += spans[i].length;
    }
    if (sum != length) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "the spans do not tile the text");
    }
  }

  p = allocator->calloc_fn(allocator->ctx, 1, sizeof *p);
  cps = ALLOC(cps, length);
  span_cp = ALLOC(span_cp, span_count + 1);
  if (p) {
    p->allocator = allocator;
  }
  if (!p || !cps || !span_cp) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the paragraph");
    goto done;
  }
  p->allocator = allocator;
  p->options = *options;
  p->provider = options->font_provider;
  if (options->ellipsis) {
    size_t len = strlen(options->ellipsis);

    for (size_t at = 0; at < len && p->ell_count < 8;) {
      uint32_t cp;

      at += guni_utf8_decode(options->ellipsis + at, len - at, &cp, NULL);
      p->ell[p->ell_count++] = cp;
    }
  }
  p->cp_byte = allocator->calloc_fn(allocator->ctx, length + 1, sizeof *p->cp_byte);
  p->styles = allocator->calloc_fn(allocator->ctx, span_count, sizeof *p->styles);
  if (!p->cp_byte || !p->styles) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the paragraph");
    goto done;
  }
  {
    // Decode, noting the code point at which every span starts.
    size_t pos = 0;
    size_t next_span = 0;
    size_t boundary = 0;   // the byte at which span next_span starts

    for (size_t s = 0; s <= span_count; s++) {
      span_cp[s] = (size_t)-1;
    }
    while (pos < length) {
      uint32_t cp;
      size_t used;

      while (next_span < span_count && boundary == pos) {
        span_cp[next_span] = n;
        boundary += spans[next_span].length;
        next_span++;
      }
      if (boundary < pos) {
        r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, pos, GFNT_GLYPH_NONE,
            "a span ends inside a character");
        goto done;
      }
      used = guni_utf8_decode(text + pos, length - pos, &cp, NULL);
      p->cp_byte[n] = pos;
      cps[n++] = cp;
      pos += used;
    }
    while (next_span < span_count && boundary == pos) {
      span_cp[next_span] = n;
      boundary += spans[next_span].length;
      next_span++;
    }
    if (next_span < span_count) {
      r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, pos, GFNT_GLYPH_NONE,
          "a span ends inside a character");
      goto done;
    }
    p->cp_byte[n] = length;
    span_cp[span_count] = n;
  }
  p->length = n;
  p->cps = cps;
  cps_owned = true;
  p->span_count = span_count;
  for (size_t i = 0; i < span_count; i++) {
    p->styles[i] = spans[i].style;
  }

  levels = ALLOC(levels, n);
  edge = ALLOC(edge, n + 1);
  opp = ALLOC(opp, n + 1);
  if (!levels || !edge || !opp) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the paragraph");
    goto done;
  }
  if (n) {
    GUNI_BidiDirection dir = options->direction == GFNT_BIDI_RTL ? GUNI_BIDI_RTL
        : options->direction == GFNT_BIDI_AUTO ? GUNI_BIDI_AUTO : GUNI_BIDI_LTR;
    size_t script_count = 0;
    GUNI_BreakOptions bo;
    size_t break_count = 0;

    if (guni_bidi_levels_with_allocator(cps, n, dir, NULL, levels, n,
            &p->para_level, NULL) != GUNI_OK) {
      r = gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
          "the text is too long for the bidirectional algorithm");
      goto done;
    }
    // A call with no room reports the count it needs, with a limit error.
    (void)guni_script_items(cps, n, NULL, 0, &script_count);
    if (script_count) {
      scripts = ALLOC(scripts, script_count);
      if (!scripts) {
        r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for the paragraph");
        goto done;
      }
      if (guni_script_items(cps, n, scripts, script_count, &script_count)
          == GUNI_OK) {
        for (size_t i = 0; i < script_count; i++) {
          edge[scripts[i].start] = true;
        }
      }
    }
    for (size_t s = 0; s < span_count; s++) {
      if (span_cp[s] < n) {
        edge[span_cp[s]] = true;
      }
    }
    for (size_t i = 1; i < n; i++) {
      if (levels[i] != levels[i - 1]) {
        edge[i] = true;
      }
    }
    edge[0] = true;
    edge[n] = true;

    memset(&bo, 0, sizeof bo);
    bo.kind = GUNI_BREAK_LINE;
    bo.tailoring = options->line_break == GFNT_LINE_BREAK_STRICT
        ? GUNI_LINE_BREAK_STRICT
        : options->line_break == GFNT_LINE_BREAK_LOOSE ? GUNI_LINE_BREAK_LOOSE
        : options->line_break == GFNT_LINE_BREAK_ANYWHERE
            ? GUNI_LINE_BREAK_ANYWHERE : GUNI_LINE_BREAK_NORMAL;
    (void)guni_break_all_codepoints(&bo, cps, n, NULL, 0, &break_count);
    if (break_count) {
      breaks = ALLOC(breaks, break_count);
      if (!breaks) {
        r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for the paragraph");
        goto done;
      }
      if (guni_break_all_codepoints(&bo, cps, n, breaks, break_count,
              &break_count) == GUNI_OK) {
        for (size_t i = 0; i < break_count; i++) {
          if (breaks[i] <= n) {
            opp[breaks[i]] = true;
          }
        }
      }
    }
  }
  if (n) {
    // Count the face runs the items will give, so the arrays can be sized: an
    // item shapes into at most one run per code point.
    total_runs = n;
    p->runs = ALLOC(p->runs, total_runs);
    p->units = ALLOC(p->units, n + 1);
    if (!p->runs || !p->units) {
      r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory for the paragraph");
      goto done;
    }
  }
  {
    // The glyph array grows as items are shaped.
    size_t glyph_cap = 0;
    size_t span = 0;

    for (size_t start = 0; start < n;) {
      size_t end = start + 1;
      GFNT_FaceRuns fr;
      GFNT_ShapeOptions so;
      GFNT_ShapeFeature * features;
      size_t feature_count;
      const GFNT_TextStyle * style;
      bool rtl;
      uint16_t upem;
      const GFNT_Face * const * flist;
      size_t fcount;
      const GFNT_Face ** owned = NULL;

      while (end < n && !edge[end]) {
        end++;
      }
      while (span + 1 < span_count && span_cp[span + 1] <= start) {
        span++;
      }
      style = &spans[span].style;
      flist = style->faces;
      fcount = style->face_count;
      rtl = (levels[start] & 1) != 0;
      memset(&so, 0, sizeof so);
      so.language = style->language;
      so.direction = rtl ? GFNT_DIRECTION_RTL : GFNT_DIRECTION_LTR;
      so.variation = style->variation;
      features = cut_features(allocator, style, span_cp[span], start, end,
          &feature_count);
      if (style->feature_count && !features) {
        r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for the features");
        goto done;
      }
      so.features = features;
      so.feature_count = feature_count;
      memset(&fr, 0, sizeof fr);
      if (p->provider.font_for) {
        GFNT_Tag item_script = gfnt_shape_script_of(cps + start, end - start);

        owned = allocator->calloc_fn(allocator->ctx,
            style->face_count + PROVIDER_MAX, sizeof *owned);
        if (!owned) {
          allocator->free_fn(allocator->ctx, features);
          r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
              "no memory for the face list");
          goto done;
        }
        memcpy(owned, style->faces, style->face_count * sizeof *owned);
        flist = owned;
        fcount = style->face_count;
        for (size_t c = start; c < end; c++) {
          bool mapped = false;
          const GFNT_Face * f;

          if (provider_skips(cps[c])) {
            continue;
          }
          for (size_t k = 0; k < fcount && !mapped; k++) {
            mapped = face_maps(owned[k], cps[c]);
          }
          if (mapped) {
            continue;
          }
          f = p->provider.font_for(p->provider.ctx, cps[c], item_script,
              style->language, style);
          if (!f) {
            continue;
          }
          for (size_t k = 0; k < fcount && f; k++) {
            if (owned[k] == f) {
              f = NULL;
            }
          }
          if (f && fcount < style->face_count + PROVIDER_MAX) {
            owned[fcount++] = f;
          }
        }
      }
      r = gfnt_faces_shape(flist, fcount, cps + start, end - start, &so,
          allocator, &fr, error);
      allocator->free_fn(allocator->ctx, features);
      if (r != GFNT_OK) {
        allocator->free_fn(allocator->ctx, owned);
        owned = NULL;
        goto done;
      }
      // The face runs come back in visual order; take them in logical order.
      for (size_t k = 0; k < fr.count && r == GFNT_OK; k++) {
        const GFNT_FaceRun * f = &fr.runs[rtl ? fr.count - 1 - k : k];
        GFNT_F16Dot16 scale;
        PRun * pr;

        if (p->run_count >= total_runs) {
          r = gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
              "more face runs than characters");
          break;
        }
        if (p->glyph_count + f->run.count > glyph_cap) {
          size_t cap = glyph_cap ? glyph_cap * 2 : 64;
          PGlyph * bigger;

          while (cap < p->glyph_count + f->run.count) {
            cap *= 2;
          }
          bigger = allocator->realloc_fn(allocator->ctx, p->glyphs,
              cap * sizeof *bigger);
          if (!bigger) {
            r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
                "no memory for the glyphs");
            break;
          }
          p->glyphs = bigger;
          glyph_cap = cap;
        }
        pr = &p->runs[p->run_count];
        memset(pr, 0, sizeof *pr);
        pr->face = flist[f->face];
        pr->span = span;
        pr->face_index = f->face;
        pr->level = levels[start];
        scale_metrics(pr->face, style, pr);
        upem = 0;
        if (gfnt_face_units_per_em(pr->face, &upem, NULL) != GFNT_OK || !upem) {
          upem = 1000;
        }
        scale = (GFNT_F16Dot16)(((int64_t)style->size * 1024 + upem / 2) / upem);
        for (size_t g = 0; g < f->run.count; g++) {
          const GFNT_ShapedGlyph * sg = &f->run.glyphs[g];
          PGlyph * pg = &p->glyphs[p->glyph_count + g];
          size_t cp = start + sg->cluster;

          pg->glyph = sg->glyph;
          pg->cluster = (uint32_t)p->cp_byte[cp < n ? cp : n];
          pg->advance = gfnt_units_to_pixels(sg->x_advance, scale);
          pg->dx = gfnt_units_to_pixels(sg->x_offset, scale);
          pg->dy = gfnt_units_to_pixels(sg->y_offset, scale);
          pg->expand = false;
          pg->tab = false;
          if (r == GFNT_OK) {
            r = glyph_ink(pg, pr->face, style, scale, allocator);
            if (r != GFNT_OK) {
              r = gfnt_error_set(error, r, 0, 0, GFNT_GLYPH_NONE,
                  "no memory for a glyph's outline");
            }
          }
        }
        {
          // Not `r = `: an out-of-memory in a glyph's outline above would be lost,
          // and the glyphs after it would be left without their ink flag.
          GFNT_Result units = add_units(p, f, start, rtl, p->run_count, p->glyph_count,
              cps, error);

          if (r == GFNT_OK) {
            r = units;
          }
        }
        p->glyph_count += f->run.count;
        p->run_count++;
      }
      gfnt_face_runs_free(&fr);
      allocator->free_fn(allocator->ctx, owned);
      if (r != GFNT_OK) {
        goto done;
      }
      start = end;
    }
  }
  {
    // Where a line may and must break, onto the units.
    for (size_t i = 0; i < p->unit_count; i++) {
      Unit * u = &p->units[i];
      uint32_t last = cps[u->cp_end - 1];

      u->opportunity = opp[u->cp_end];
      u->hard = is_newline(last)
          && !(last == 0x0D && u->cp_end < n && cps[u->cp_end] == 0x0A);
    }
  }
  if (p->tab_width < 0) {
    if (options->tab_width > 0) {
      p->tab_width = options->tab_width;
    }
    else {
      uint32_t space = 0x20;
      PGlyph g[ELL_GLYPHS];
      size_t gn = 0;
      GFNT_F26Dot6 w = 0;
      PRun tr;

      r = shape_string(p, 0, &space, 1, g, &gn, &w, &tr, error);
      if (r != GFNT_OK) {
        goto done;
      }
      p->tab_width = w > 0 ? 8 * w : 2 * spans[0].style.size;
    }
  }
  if (n == 0) {
    p->fallback_metrics.face = spans[0].style.faces[0];
    scale_metrics(p->fallback_metrics.face, &spans[0].style, &p->fallback_metrics);
  }
  *out = p;
  p = NULL;
done:
  if (!cps_owned) {
    allocator->free_fn(allocator->ctx, cps);
  }
  allocator->free_fn(allocator->ctx, levels);
  allocator->free_fn(allocator->ctx, span_cp);
  allocator->free_fn(allocator->ctx, edge);
  allocator->free_fn(allocator->ctx, opp);
  allocator->free_fn(allocator->ctx, breaks);
  allocator->free_fn(allocator->ctx, scripts);
  gfnt_paragraph_free(p);
  return r;
}

/* ------------------------------------------------------------ content widths */

GFNT_Result gfnt_paragraph_min_content(const GFNT_Paragraph * p,
    GFNT_F26Dot6 * out) {
  GFNT_F26Dot6 total = 0, fit = 0, widest = 0;

  if (!p || !out) {
    return GFNT_ERR_INVALID;
  }
  for (size_t j = 0; j < p->unit_count; j++) {
    const Unit * u = &p->units[j];

    total += eff_width(p, u, total);
    if (!u->space) {
      fit = total;
    }
    if (fit > widest) {
      widest = fit;
    }
    if (u->hard || u->opportunity) {
      total = fit = 0;
    }
  }
  *out = widest;
  return GFNT_OK;
}

GFNT_Result gfnt_paragraph_max_content(const GFNT_Paragraph * p,
    GFNT_F26Dot6 * out) {
  GFNT_F26Dot6 total = 0, fit = 0, widest = 0;

  if (!p || !out) {
    return GFNT_ERR_INVALID;
  }
  for (size_t j = 0; j < p->unit_count; j++) {
    const Unit * u = &p->units[j];

    total += eff_width(p, u, total);
    if (!u->space) {
      fit = total;
    }
    if (fit > widest) {
      widest = fit;
    }
    if (u->hard) {
      total = fit = 0;
    }
  }
  *out = widest;
  return GFNT_OK;
}

/* ------------------------------------------------------------------ layout */

void gfnt_layout_free(GFNT_Layout * layout) {
  if (!layout || !layout->allocator) {
    return;
  }
  layout->allocator->free_fn(layout->allocator->ctx, layout->lines);
  layout->allocator->free_fn(layout->allocator->ctx, layout->storage_);
  memset(layout, 0, sizeof *layout);
}

/** A slice of a line: units of one run, at one level. */
typedef struct {
  const PRun * pr;
  const PGlyph * src;
  uint8_t level;
  bool hang;
  bool ellipsis;
  size_t g_lo, g_hi;
  size_t b_lo, b_hi;   // the bytes of text it stands for
  GFNT_F26Dot6 advance;
} Slice;

/** Cut a line [ua, ub) of units into line starts. Returns how many lines. */
static size_t break_lines(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    size_t * starts) {
  size_t lines = 0;
  size_t i = 0;

  if (p->unit_count == 0) {
    starts[0] = 0;
    return 1;
  }
  while (i < p->unit_count) {
    GFNT_F26Dot6 total = 0, fit = 0;
    size_t cand = (size_t)-1;
    size_t end = p->unit_count;
    bool bounded = width != GFNT_LAYOUT_UNBOUNDED;

    starts[lines++] = i;
    for (size_t j = i; j < p->unit_count; j++) {
      const Unit * u = &p->units[j];

      total += eff_width(p, u, total);
      if (!u->space) {
        fit = total;
      }
      if (u->hard) {
        end = j + 1;
        break;
      }
      if (bounded && fit > width && cand != (size_t)-1) {
        end = cand + 1;
        break;
      }
      if (u->opportunity && j + 1 < p->unit_count) {
        if (bounded && fit > width) {
          end = j + 1;
          break;
        }
        cand = j;
      }
    }
    i = end;
  }
  // A text ending in a newline has an empty last line.
  if (p->units[p->unit_count - 1].hard) {
    starts[lines++] = p->unit_count;
  }
  return lines;
}

/** Shape a short string in a span's style and first face. */
static GFNT_Result shape_string(const GFNT_Paragraph * p, size_t span,
    const uint32_t * text, size_t text_count,
    PGlyph * glyphs, size_t * count, GFNT_F26Dot6 * total, PRun * run,
    GFNT_Error * error) {
  const GFNT_TextStyle * style = &p->styles[span];
  GFNT_ShapeOptions so;
  GFNT_FaceRuns fr;
  GFNT_Result r;
  uint16_t upem = 0;
  GFNT_F16Dot16 scale;

  memset(&so, 0, sizeof so);
  memset(&fr, 0, sizeof fr);
  so.language = style->language;
  so.variation = style->variation;
  so.direction = (p->para_level & 1) ? GFNT_DIRECTION_RTL : GFNT_DIRECTION_LTR;
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
  if (gfnt_face_units_per_em(run->face, &upem, NULL) != GFNT_OK || !upem) {
    upem = 1000;
  }
  scale = (GFNT_F16Dot16)(((int64_t)style->size * 1024 + upem / 2) / upem);
  for (size_t k = 0; k < fr.count; k++) {
    for (size_t g = 0; g < fr.runs[k].run.count && *count < ELL_GLYPHS; g++) {
      const GFNT_ShapedGlyph * sg = &fr.runs[k].run.glyphs[g];
      PGlyph * pg = &glyphs[(*count)++];

      memset(pg, 0, sizeof *pg);
      pg->glyph = sg->glyph;
      pg->has_ink = false;
      pg->advance = gfnt_units_to_pixels(sg->x_advance, scale);
      pg->dx = gfnt_units_to_pixels(sg->x_offset, scale);
      pg->dy = gfnt_units_to_pixels(sg->y_offset, scale);
      *total += pg->advance;
      if (r == GFNT_OK) {
        r = glyph_ink(pg, run->face, style, scale, p->allocator);
      }
    }
  }
  gfnt_face_runs_free(&fr);
  return r;
}

GFNT_Result gfnt_paragraph_layout(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    GFNT_Layout * out, GFNT_Error * error) {
  const GFNT_Allocator * allocator;
  size_t * starts = NULL;
  Slice * slices = NULL;
  size_t * order = NULL;
  uint8_t * slice_levels = NULL;
  GFNT_F26Dot6 * ew = NULL;       // each unit's width in its line, tabs resolved
  GFNT_F26Dot6 * tabadv = NULL;   // a tab glyph's advance, by glyph index
  GFNT_Rect ink_all = {0, 0, 0, 0};
  GFNT_Line * lines = NULL;
  uint8_t * storage = NULL;
  GFNT_LineRun * runs;
  GFNT_PlacedGlyph * placed;
  size_t line_count, total_lines, run_used = 0, glyph_used = 0;
  bool more = false, any_truncated = false;
  size_t runs_bytes, total_bytes;
  GFNT_F26Dot6 y = 0, widest = 0;
  bool para_rtl;
  GFNT_Result r = GFNT_OK;

  gfnt_error_clear(error);
  if (!p || !out || width < 0) {
    return GFNT_ERR_INVALID;
  }
  allocator = p->allocator;
  para_rtl = (p->para_level & 1) != 0;
  starts = allocator->calloc_fn(allocator->ctx, p->unit_count + 2, sizeof *starts);
  slices = allocator->calloc_fn(allocator->ctx, p->unit_count + 1, sizeof *slices);
  order = allocator->calloc_fn(allocator->ctx, p->unit_count + 1, sizeof *order);
  slice_levels = allocator->calloc_fn(allocator->ctx, p->unit_count + 1, 1);
  ew = allocator->calloc_fn(allocator->ctx, p->unit_count + 1, sizeof *ew);
  tabadv = allocator->calloc_fn(allocator->ctx, p->glyph_count + 1, sizeof *tabadv);
  if (!starts || !slices || !order || !slice_levels || !ew || !tabadv) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the layout");
    goto done;
  }
  line_count = break_lines(p, width, starts);
  starts[line_count] = p->unit_count;
  total_lines = line_count;
  if (p->options.max_lines && line_count > p->options.max_lines) {
    line_count = p->options.max_lines;
    more = true;
  }
  runs_bytes = (p->unit_count + 2) * sizeof *runs;
  total_bytes = runs_bytes + (p->glyph_count + ELL_GLYPHS + 1) * sizeof *placed;
  lines = allocator->calloc_fn(allocator->ctx, line_count, sizeof *lines);
  storage = allocator->calloc_fn(allocator->ctx, total_bytes, 1);
  if (!lines || !storage) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the layout");
    goto done;
  }
  runs = (GFNT_LineRun *)storage;
  placed = (GFNT_PlacedGlyph *)(storage + runs_bytes);

  for (size_t li = 0; li < line_count; li++) {
    size_t ua = starts[li];
    size_t ub = starts[li + 1];
    GFNT_Line * line = &lines[li];
    size_t hang_from;
    size_t ns = 0;
    GFNT_F26Dot6 asc = 0, desc = 0, gap = 0;
    GFNT_F26Dot6 cursor = 0, lead = 0;
    bool seen_text = false;
    bool ink_set = false;
    GFNT_F26Dot6 ink_l = 0, ink_t = 0, ink_r = 0, ink_b = 0;
    PGlyph ell_g[ELL_GLYPHS];
    size_t ell_n = 0;
    GFNT_F26Dot6 ell_w = 0, natural = 0, extra = 0, extra_rem = 0;
    size_t expanded = 0;
    PRun ell_run;
    bool trunc_line = false;
    bool bounded = width != GFNT_LAYOUT_UNBOUNDED;

    {
      GFNT_F26Dot6 pos = 0;

      for (size_t j = ua; j < ub; j++) {
        ew[j] = eff_width(p, &p->units[j], pos);
        pos += ew[j];
        if (p->units[j].tab) {
          tabadv[p->units[j].g_lo] = ew[j];
        }
      }
    }
    hang_from = ub;
    while (hang_from > ua && p->units[hang_from - 1].space) {
      hang_from--;
    }
    for (size_t j = ua; j < hang_from; j++) {
      natural += ew[j];
    }
    if (p->options.max_lines && p->ell_count && li + 1 == line_count
        && (more || (bounded && natural > width))) {
      size_t span = ub > ua ? p->runs[p->units[ub - 1].run].span
                            : (ua < p->unit_count ? p->runs[p->units[ua].run].span
                                                  : 0);

      r = shape_string(p, span, p->ell, p->ell_count, ell_g, &ell_n, &ell_w,
          &ell_run, error);
      if (r != GFNT_OK) {
        goto done;
      }
      if (bounded) {
        GFNT_F26Dot6 room = width - ell_w, used = 0;
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
      any_truncated = true;
    }
    if (more && li + 1 == line_count) {
      any_truncated = true;
    }
    if (p->options.align == GFNT_ALIGN_JUSTIFY && bounded && !trunc_line
        && li + 1 < total_lines && ub > ua && !p->units[ub - 1].hard) {
      size_t spaces = 0;
      GFNT_F26Dot6 slack = width - natural;

      for (size_t j = ua; j < hang_from; j++) {
        if (p->units[j].space && p->units[j].g_hi > p->units[j].g_lo) {
          spaces++;
        }
      }
      if (spaces && slack > 0) {
        extra = slack / (GFNT_F26Dot6)spaces;
        extra_rem = slack % (GFNT_F26Dot6)spaces;
        line->justified = true;
      }
    }
    // Slices: a run of units of one face and level.
    for (size_t j = ua; j < ub; j++) {
      const Unit * u = &p->units[j];
      const PRun * pr = &p->runs[u->run];
      bool hang = j >= hang_from;
      uint8_t level = hang ? p->para_level : pr->level;
      Slice * s = ns ? &slices[ns - 1] : NULL;

      if (pr->ascent > asc) asc = pr->ascent;
      if (pr->descent > desc) desc = pr->descent;
      if (pr->gap > gap) gap = pr->gap;
      if (!s || s->pr != pr || s->level != level || s->hang != hang) {
        s = &slices[ns++];
        s->pr = pr;
        s->src = p->glyphs;
        s->ellipsis = false;
        s->level = level;
        s->hang = hang;
        s->g_lo = u->g_lo;
        s->g_hi = u->g_hi;
        s->b_lo = p->cp_byte[u->cp_start];
        s->b_hi = p->cp_byte[u->cp_end];
        s->advance = 0;
      }
      if (p->cp_byte[u->cp_start] < s->b_lo) s->b_lo = p->cp_byte[u->cp_start];
      if (p->cp_byte[u->cp_end] > s->b_hi) s->b_hi = p->cp_byte[u->cp_end];
      if (u->g_lo < s->g_lo) s->g_lo = u->g_lo;
      if (u->g_hi > s->g_hi) s->g_hi = u->g_hi;
    }
    if (trunc_line) {
      size_t at = ub > ua ? p->cp_byte[p->units[ub - 1].cp_end]
                          : (ua < p->unit_count ? p->cp_byte[p->units[ua].cp_start]
                                                : p->cp_byte[p->length]);
      Slice * s = &slices[ns++];

      for (size_t g = 0; g < ell_n; g++) {
        ell_g[g].cluster = (uint32_t)at;
      }
      s->pr = &ell_run;
      s->src = ell_g;
      s->ellipsis = true;
      s->level = p->para_level;
      s->hang = false;
      s->g_lo = 0;
      s->g_hi = ell_n;
      s->b_lo = s->b_hi = at;
      s->advance = 0;
    }
    if (ua == ub) {
      const PRun * pr = ua < p->unit_count ? &p->runs[p->units[ua].run]
                                           : &p->fallback_metrics;
      const PRun * src = p->run_count ? (ua > 0 ? &p->runs[p->units[ua - 1].run] : pr)
                                      : pr;

      asc = src->ascent;
      desc = src->descent;
      gap = src->gap;
    }
    for (size_t k = 0; k < ns; k++) {
      slice_levels[k] = slices[k].level;
    }
    if (ns && guni_bidi_reorder(slice_levels, ns, order, p->unit_count + 1)
            != GUNI_OK) {
      r = gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
          "the line could not be put in visual order");
      goto done;
    }
    line->runs = &runs[run_used];
    line->run_count = ns;
    line->start = ua < p->unit_count ? p->cp_byte[p->units[ua].cp_start]
                                     : p->cp_byte[p->length];
    line->end = ub > ua ? p->cp_byte[p->units[ub - 1].cp_end] : line->start;
    line->top = y;
    line->ascent = asc;
    line->descent = desc;
    line->height = asc + desc + gap;
    line->baseline = y + gap / 2 + asc;
    for (size_t v = 0; v < ns; v++) {
      const Slice * s = &slices[order[v]];
      const PRun * pr = s->pr;
      GFNT_LineRun * lr = &runs[run_used++];

      lr->face = pr->face;
      lr->span = pr->span;
      lr->face_index = pr->face_index;
      lr->fallback = pr->face_index != 0;
      lr->ellipsis = s->ellipsis;
      lr->level = s->level;
      lr->glyphs = &placed[glyph_used];
      lr->glyph_count = s->g_hi - s->g_lo;
      lr->x = cursor;
      for (size_t g = s->g_lo; g < s->g_hi; g++) {
        const PGlyph * pg = &s->src[g];
        GFNT_PlacedGlyph * pl = &placed[glyph_used++];
        GFNT_F26Dot6 adv = pg->tab && s->src == p->glyphs ? tabadv[g] : pg->advance;

        if (line->justified && pg->expand && !s->hang) {
          adv += extra + (expanded < (size_t)extra_rem ? 1 : 0);
          expanded++;
        }

        pl->glyph = pg->glyph;
        pl->cluster = pg->cluster;
        pl->pen_x = cursor;
        pl->x = cursor + pg->dx;
        pl->y = line->baseline - pg->dy;
        pl->advance = adv;
        cursor += adv;
        if (pg->has_ink) {
          GFNT_F26Dot6 l = pl->x + pg->ix0, t = pl->y - pg->iy1;
          GFNT_F26Dot6 rr = pl->x + pg->ix1, b = pl->y - pg->iy0;

          if (!ink_set) {
            ink_l = l; ink_t = t; ink_r = rr; ink_b = b;
            ink_set = true;
          }
          else {
            if (l < ink_l) ink_l = l;
            if (t < ink_t) ink_t = t;
            if (rr > ink_r) ink_r = rr;
            if (b > ink_b) ink_b = b;
          }
        }
      }
      lr->advance = cursor - lr->x;
      lr->start = s->b_lo;
      lr->end = s->b_hi;
      if (s->hang && !seen_text) {
        lead += lr->advance;
      }
      if (!s->hang) {
        seen_text = true;
        line->width += lr->advance;
      }
    }
    line->truncated = trunc_line;
    if (ink_set) {
      line->ink.x = ink_l;
      line->ink.y = ink_t;
      line->ink.width = ink_r - ink_l;
      line->ink.height = ink_b - ink_t;
    }
    line->x = -lead;   // made absolute by the alignment pass
    if (line->width > widest) {
      widest = line->width;
    }
    y += line->height;
  }
  {
    GFNT_F26Dot6 avail = width == GFNT_LAYOUT_UNBOUNDED ? widest : width;
    bool ink_any = false;

    for (size_t li = 0; li < line_count; li++) {
      GFNT_Line * line = &lines[li];
      GFNT_F26Dot6 slack = avail - line->width;
      GFNT_F26Dot6 off;
      GFNT_Align a = p->options.align;
      GFNT_F26Dot6 shift;

      if (a == GFNT_ALIGN_JUSTIFY) {
        a = GFNT_ALIGN_START;
      }
      if (a == GFNT_ALIGN_START) {
        a = para_rtl ? GFNT_ALIGN_RIGHT : GFNT_ALIGN_LEFT;
      }
      else if (a == GFNT_ALIGN_END) {
        a = para_rtl ? GFNT_ALIGN_LEFT : GFNT_ALIGN_RIGHT;
      }
      if (slack < 0) {
        off = para_rtl ? slack : 0;
      }
      else {
        off = a == GFNT_ALIGN_RIGHT ? slack : a == GFNT_ALIGN_CENTER ? slack / 2 : 0;
      }
      shift = off + line->x;   // line->x held minus the leading hang
      line->x = off;
      if (line->ink.width || line->ink.height) {
        line->ink.x += shift;
        if (ink_any) {
          GFNT_F26Dot6 r1 = ink_all.x + ink_all.width, b1 = ink_all.y + ink_all.height;
          GFNT_F26Dot6 r2 = line->ink.x + line->ink.width;
          GFNT_F26Dot6 b2 = line->ink.y + line->ink.height;

          if (line->ink.x < ink_all.x) ink_all.x = line->ink.x;
          if (line->ink.y < ink_all.y) ink_all.y = line->ink.y;
          ink_all.width = (r2 > r1 ? r2 : r1) - ink_all.x;
          ink_all.height = (b2 > b1 ? b2 : b1) - ink_all.y;
        }
        else {
          ink_all = line->ink;
          ink_any = true;
        }
      }
      for (size_t k = 0; k < line->run_count; k++) {
        GFNT_LineRun * lr = (GFNT_LineRun *)&line->runs[k];

        lr->x += shift;
        for (size_t g = 0; g < lr->glyph_count; g++) {
          GFNT_PlacedGlyph * pl = (GFNT_PlacedGlyph *)&lr->glyphs[g];

          pl->pen_x += shift;
          pl->x += shift;
        }
      }
    }
  }
  out->lines = lines;
  out->line_count = line_count;
  out->width = width == GFNT_LAYOUT_UNBOUNDED ? widest : width;
  out->height = y;
  out->truncated = any_truncated;
  out->ink = ink_all;
  out->allocator = allocator;
  out->storage_ = storage;
  lines = NULL;
  storage = NULL;
done:
  allocator->free_fn(allocator->ctx, starts);
  allocator->free_fn(allocator->ctx, slices);
  allocator->free_fn(allocator->ctx, order);
  allocator->free_fn(allocator->ctx, slice_levels);
  allocator->free_fn(allocator->ctx, ew);
  allocator->free_fn(allocator->ctx, tabadv);
  allocator->free_fn(allocator->ctx, lines);
  allocator->free_fn(allocator->ctx, storage);
  return r;
}
