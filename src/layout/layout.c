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

/**
 * Whether a character is one that justification opens gaps beside as it does between
 * ideographs. The set is what Chromium 154 does, measured block by block on a font made for the
 * purpose (css_layout_diff.py): the CJK radicals, symbols and punctuation, kana, bopomofo,
 * enclosed and compatibility forms, the ideographs and their extensions, the vertical and
 * small-form variants and the full- and half-width forms are in; Hangul, Yi, the katakana
 * phonetic extensions, and every other script are not.
 */
static bool is_ideographic(uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x2FFF)
      || (cp >= 0x3001 && cp <= 0x30FF)    // U+3000 is a space
      || (cp >= 0x3100 && cp <= 0x312F)
      || (cp >= 0x3190 && cp <= 0x319F)
      || (cp >= 0x3200 && cp <= 0x4DBF)
      || (cp >= 0x4E00 && cp <= 0x9FFF)
      || (cp >= 0xF900 && cp <= 0xFAFF)
      || (cp >= 0xFE10 && cp <= 0xFE1F)
      || (cp >= 0xFE30 && cp <= 0xFE6F)
      || (cp >= 0xFF01 && cp <= 0xFF9F)
      || (cp >= 0xFFE0 && cp <= 0xFFEE)
      || (cp >= 0x1B000 && cp <= 0x1B16F)
      || (cp >= 0x1F200 && cp <= 0x1F2FF)
      || (cp >= 0x20000 && cp <= 0x3FFFF);
}

#define PROVIDER_MAX 8
#define ELL_GLYPHS 16

static GFNT_Result shape_string(const GFNT_Paragraph * p, size_t span,
    const uint32_t * text, size_t text_count, PGlyph * glyphs, size_t * count,
    GFNT_F26Dot6 * total, PRun * run, int force, GFNT_Error * error);
static uint8_t string_orientation(const GFNT_Paragraph * p, size_t span, uint32_t cp);
static int hyphen_orientation(const GFNT_Paragraph * p);

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
  allocator->free_fn(allocator->ctx, p->hyphen_width);
  allocator->free_fn(allocator->ctx, p->hyphen_cp);
  allocator->free_fn(allocator->ctx, p);
}

/** Append the units of one face run, in logical order. */
static GFNT_Result add_units(GFNT_Paragraph * p, const GFNT_FaceRun * fr,
    size_t item_start, bool rtl, bool stored_reversed, size_t run, size_t glyph_base,
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
    // Upright text at an odd level is stored visually, which is the shaper's order reversed.
    size_t cluster = item_start
        + fr->run.glyphs[stored_reversed ? count - 1 - g : g].cluster;
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

static bool is_word_cp(uint32_t cp) {
  return (guni_general_category_mask(cp) & (GUNI_GC_MASK_L | GUNI_GC_MASK_M)) != 0;
}

/**
 * Mark the places a line may break with a hyphen added: after a soft hyphen (unless hyphens are
 * off, when a soft hyphen is no place at all), and where the hyphenator says inside a word.
 * Then set the width and the character of the hyphen for each span that has such a place.
 */
static GFNT_Result mark_hyphens(GFNT_Paragraph * p, const char * text, const size_t * span_cp,
    GFNT_Error * error) {
  const GFNT_Allocator * a = p->allocator;
  const GFNT_Hyphenator * hy = &p->options.hyphenator;
  const bool automatic = p->options.hyphens == GFNT_HYPHENS_AUTO && hy->points;
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
      const bool ideograph = u->cp_end >= 2 && is_ideographic(p->cps[u->cp_end - 2]);

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

        if (allowed[k] && unit != (size_t)-1 && !p->units[unit].hard
            && !p->units[unit].opportunity) {
          p->units[unit].opportunity = true;
          p->units[unit].hyphen = !is_ideographic(p->cps[i - 1]);
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
    p->hyphen_cp[i] = face_maps(p->styles[i].faces[0], 0x2010) ? 0x2010 : 0x2D;
    r = shape_string(p, i, &p->hyphen_cp[i], 1, g, &count, &width, &run, hyphen_orientation(p), error);
    p->hyphen_width[i] = width;
    if (count > p->hyphen_glyphs) {
      p->hyphen_glyphs = count;
    }
  }
  a->free_fn(a->ctx, wanted);
  return r;
}

/* ------------------------------------------------------------ vertical orientation */

#define ORI_NONE 0
#define ORI_UPRIGHT 1
#define ORI_SIDEWAYS 2

/**
 * Whether the face list gives @p cp another glyph when it is set top to bottom (a `vert` or `vrt2`
 * form) than when it is set along a row.
 */
static bool has_vertical_form(const GFNT_Paragraph * p, const GFNT_TextStyle * style,
    uint32_t cp) {
  GFNT_Result r;
  uint32_t glyph[2] = {0, 0};
  uint32_t face[2] = {0, 0};

  for (int k = 0; k < 2; k++) {
    GFNT_ShapeOptions so;
    GFNT_FaceRuns fr;

    memset(&so, 0, sizeof so);
    memset(&fr, 0, sizeof fr);
    so.language = style->language;
    so.variation = style->variation;
    so.direction = k ? GFNT_DIRECTION_TTB : GFNT_DIRECTION_LTR;
    r = gfnt_faces_shape(style->faces, style->face_count, &cp, 1, &so, p->allocator, &fr, NULL);
    if (r != GFNT_OK) {
      return false;
    }
    if (fr.count && fr.runs[0].run.count) {
      glyph[k] = fr.runs[0].run.glyphs[0].glyph;
      face[k] = (uint32_t)fr.runs[0].face;
    }
    gfnt_face_runs_free(&fr);
  }
  return glyph[0] != glyph[1] && face[0] == face[1];
}

/** Whether a character takes the orientation of the character it follows. */
static bool inherits_orientation(uint32_t cp) {
  if (cp == 0x200C || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F)
      || (cp >= 0xE0100 && cp <= 0xE01EF)) {
    return true;
  }
  return (guni_general_category_mask(cp) & GUNI_GC_MASK_M) != 0;
}

/** How a character sits in vertical text, by the paragraph's orientation option and UAX #50. */
static uint8_t char_orientation(const GFNT_Paragraph * p, const GFNT_TextStyle * style,
    uint32_t cp) {
  if (p->mode == GFNT_WRITING_SIDEWAYS_RL || p->mode == GFNT_WRITING_SIDEWAYS_LR
      || p->options.text_orientation == GFNT_ORIENT_SIDEWAYS) {
    return ORI_SIDEWAYS;
  }
  if (p->options.text_orientation == GFNT_ORIENT_UPRIGHT) {
    return ORI_UPRIGHT;
  }
  if (p->options.text_orientation == GFNT_ORIENT_MIXED_PANGO) {
    // Pango's natural gravity: UAX #50 with every transformed-rotated character turned whether
    // or not the face has a vertical form for it, three symbols turned that UAX #50 sets
    // upright, and one letter set upright that it turns.
    if (cp == 0x0D4E) {
      return ORI_UPRIGHT;
    }
    if (cp == 0x2B97 || cp == 0x2E50 || cp == 0x2E51
        || guni_vertical_orientation(cp) == GUNI_VO_TRANSFORMED_ROTATED) {
      return ORI_SIDEWAYS;
    }
  }
  switch (guni_vertical_orientation(cp)) {
    case GUNI_VO_UPRIGHT:
    case GUNI_VO_TRANSFORMED_UPRIGHT:
      return ORI_UPRIGHT;
    case GUNI_VO_TRANSFORMED_ROTATED:
      return has_vertical_form(p, style, cp) ? ORI_UPRIGHT : ORI_SIDEWAYS;
    default:
      return ORI_SIDEWAYS;
  }
}

static uint8_t string_orientation(const GFNT_Paragraph * p, size_t span, uint32_t cp) {
  if (!p->vertical) {
    return GFNT_ORIENT_RUN_HORIZONTAL;
  }
  return char_orientation(p, &p->styles[span], cp) == ORI_UPRIGHT ? GFNT_ORIENT_RUN_UPRIGHT
                                                                    : GFNT_ORIENT_RUN_CW;
}

/**
 * How the hyphen a line ends in sits. In vertical text that reads CSS's way (the columns of
 * `vertical-rl` and `vertical-lr`, orientation mixed or upright) Chromium draws it upright, on
 * the vertical advance, however the letters before it sit; everywhere else it is set as the
 * character it is, which for a hyphen is sideways (and Pango's way, whatever the orientation).
 * -1 means that.
 */
static int hyphen_orientation(const GFNT_Paragraph * p) {
  if ((p->mode == GFNT_WRITING_VERTICAL_RL || p->mode == GFNT_WRITING_VERTICAL_LR)
      && (p->options.text_orientation == GFNT_ORIENT_MIXED
          || p->options.text_orientation == GFNT_ORIENT_UPRIGHT)) {
    return GFNT_ORIENT_RUN_UPRIGHT;
  }
  return -1;
}

GFNT_Result gfnt_paragraph_create(const char * text, size_t length,
    const GFNT_TextSpan * spans, size_t span_count,
    const GFNT_ParagraphOptions * options, const GFNT_Allocator * allocator,
    GFNT_Paragraph ** out, GFNT_Error * error) {
  static const GFNT_ParagraphOptions zero;
  GFNT_Paragraph * p = NULL;
  uint32_t * cps = NULL;
  uint8_t * levels = NULL;
  uint8_t * ori = NULL;         // per code point, in vertical text: ORI_UPRIGHT or ORI_SIDEWAYS
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
      || (unsigned)options->line_break > GFNT_LINE_BREAK_ANYWHERE
      || (unsigned)options->hyphens > GFNT_HYPHENS_MANUAL_PANGO
      || (unsigned)options->justify > GFNT_JUSTIFY_NONE
      || (unsigned)options->writing_mode > GFNT_WRITING_SIDEWAYS_LR
      || (unsigned)options->text_orientation > GFNT_ORIENT_MIXED_PANGO) {
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
  p->mode = options->writing_mode;
  p->vertical = options->writing_mode != GFNT_WRITING_HORIZONTAL_TB;
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
  ori = ALLOC(ori, n);
  edge = ALLOC(edge, n + 1);
  opp = ALLOC(opp, n + 1);
  if (!levels || !ori || !edge || !opp) {
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
    if (p->vertical && options->text_orientation == GFNT_ORIENT_UPRIGHT
        && p->mode != GFNT_WRITING_SIDEWAYS_RL && p->mode != GFNT_WRITING_SIDEWAYS_LR) {
      // Upright text reads as left to right whatever its characters are (CSS Writing Modes).
      memset(levels, 0, n);
      p->para_level = 0;
    }
    if (p->vertical) {
      size_t sp = 0;

      for (size_t i = 0; i < n; i++) {
        while (sp + 1 < span_count && span_cp[sp + 1] <= i) {
          sp++;
        }
        if (i > 0 && inherits_orientation(cps[i])) {
          ori[i] = ori[i - 1];
        }
        else if (options->text_orientation == GFNT_ORIENT_MIXED_PANGO
            && (cps[i] == 0x261D || (cps[i] >= 0x2795 && cps[i] <= 0x2797) || cps[i] == 0x27B0
                || cps[i] == 0x27BF)) {
          // Pango sets these emoji-like symbols with the text they follow.
          ori[i] = i > 0 ? ori[i - 1] : ORI_SIDEWAYS;
        }
        else {
          ori[i] = char_orientation(p, &spans[sp].style, cps[i]);
        }
      }
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
      if (levels[i] != levels[i - 1] || (p->vertical && ori[i] != ori[i - 1])) {
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
      bool upright;
      bool rev;
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
      upright = p->vertical && ori[start] == ORI_UPRIGHT;
      rtl = !upright && (levels[start] & 1) != 0;
      rev = upright && (levels[start] & 1) != 0;
      memset(&so, 0, sizeof so);
      so.language = style->language;
      so.direction = upright ? GFNT_DIRECTION_TTB
                             : rtl ? GFNT_DIRECTION_RTL : GFNT_DIRECTION_LTR;
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
        pr->orient = p->vertical ? (upright ? GFNT_ORIENT_RUN_UPRIGHT : GFNT_ORIENT_RUN_CW)
                                 : GFNT_ORIENT_RUN_HORIZONTAL;
        scale_metrics(pr->face, style, pr);
        upem = 0;
        if (gfnt_face_units_per_em(pr->face, &upem, NULL) != GFNT_OK || !upem) {
          upem = 1000;
        }
        scale = (GFNT_F16Dot16)(((int64_t)style->size * 1024 + upem / 2) / upem);
        for (size_t g = 0; g < f->run.count; g++) {
          // Upright text at an odd level is stored the way a right-to-left run is: visually.
          const GFNT_ShapedGlyph * sg = &f->run.glyphs[rev ? f->run.count - 1 - g : g];
          PGlyph * pg = &p->glyphs[p->glyph_count + g];
          size_t cp = start + sg->cluster;

          pg->glyph = sg->glyph;
          pg->cluster = (uint32_t)p->cp_byte[cp < n ? cp : n];
          pg->advance = gfnt_units_to_pixels(upright ? -sg->y_advance : sg->x_advance, scale);
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
          GFNT_Result units = add_units(p, f, start, rtl || rev, rev, p->run_count,
              p->glyph_count, cps, error);

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
  r = mark_hyphens(p, text, span_cp, error);
  if (r != GFNT_OK) {
    goto done;
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

      r = shape_string(p, 0, &space, 1, g, &gn, &w, &tr, -1, error);
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
  allocator->free_fn(allocator->ctx, ori);
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

    GFNT_F26Dot6 shown;

    total += eff_width(p, u, total);
    if (!u->space) {
      fit = total;
    }
    // A stretch that ends at a hyphenation point ends in the hyphen; the end of the text is not one.
    shown = fit + (u->hyphen && u->opportunity && j + 1 < p->unit_count
                       ? p->hyphen_width[p->runs[u->run].span] : 0);
    if (shown > widest) {
      widest = shown;
    }
    if (u->hard || u->opportunity) {
      total = fit = 0;
    }
  }
  {
    // A tab at the start of a stretch is a whole tab stop wide there and only the way to the next
    // stop in the unbroken line, so the stretches can add up to more than the line: a width at
    // which the line does not wrap needs no more than the line.
    GFNT_F26Dot6 max = 0;

    if (gfnt_paragraph_max_content(p, &max) == GFNT_OK && widest > max) {
      widest = max;
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
  bool hyphen;
  size_t g_lo, g_hi;
  size_t b_lo, b_hi;   // the bytes of text it stands for
  GFNT_F26Dot6 advance;
} Slice;

/**
 * Cut the units into lines. Fills @p starts with where each line begins and @p hyphened with
 * whether it ends at a hyphenation point, so that a hyphen is added. Returns how many lines.
 *
 * @p cand_at and @p cand_fit are scratch for the places a line could break so far, each as many
 * as there are units.
 */
static size_t break_lines(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    size_t * starts, bool * hyphened, size_t * cand_at, GFNT_F26Dot6 * cand_fit) {
  size_t lines = 0;
  size_t i = 0;

  if (p->unit_count == 0) {
    starts[0] = 0;
    hyphened[0] = false;
    return 1;
  }
  while (i < p->unit_count) {
    GFNT_F26Dot6 total = 0, fit = 0;
    size_t cands = 0;
    size_t end = p->unit_count;
    bool hyphen = false;
    bool bounded = width != GFNT_LAYOUT_UNBOUNDED;

    hyphened[lines] = false;
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
      if (bounded && fit > width && cands) {
        // Over the width: break at the last place where what stays, and the hyphen if that place
        // wants one, fit; failing that at the first place, which overflows the least.
        size_t k = cands;

        while (k > 0) {
          const Unit * c = &p->units[cand_at[k - 1]];

          if (!c->hyphen
              || cand_fit[k - 1] + p->hyphen_width[p->runs[c->run].span] <= width) {
            break;
          }
          k--;
        }
        k = k ? k - 1 : 0;
        end = cand_at[k] + 1;
        hyphen = p->units[cand_at[k]].hyphen;
        break;
      }
      if (u->opportunity && j + 1 < p->unit_count) {
        if (bounded && fit > width) {
          end = j + 1;
          hyphen = u->hyphen;
          break;
        }
        cand_at[cands] = j;
        cand_fit[cands++] = fit;
      }
    }
    hyphened[lines - 1] = hyphen;
    i = end;
  }
  // A text ending in a newline has an empty last line.
  if (p->units[p->unit_count - 1].hard) {
    hyphened[lines] = false;
    starts[lines++] = p->unit_count;
  }
  return lines;
}

/** Shape a short string in a span's style and first face. */
static GFNT_Result shape_string(const GFNT_Paragraph * p, size_t span,
    const uint32_t * text, size_t text_count,
    PGlyph * glyphs, size_t * count, GFNT_F26Dot6 * total, PRun * run, int force,
    GFNT_Error * error) {
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
      : string_orientation(p, span, text[0]) == GFNT_ORIENT_RUN_UPRIGHT);
  so.direction = upright ? GFNT_DIRECTION_TTB
                         : (p->para_level & 1) ? GFNT_DIRECTION_RTL : GFNT_DIRECTION_LTR;
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
      : force >= 0 ? (uint8_t)force : string_orientation(p, span, text[0]);
  scale_metrics(run->face, style, run);
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
      pg->advance = gfnt_units_to_pixels(upright ? -sg->y_advance : sg->x_advance, scale);
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

      if (is_ideographic(p->cps[u->cp_start])) {
        // After an ideograph, whatever follows (a space included).
        gjf[rtl ? first : last] |= rtl ? 1 : 2;
        n++;
      }
      else if (!v->space && !v->tab && v->g_hi > v->g_lo
          && is_ideographic(p->cps[v->cp_start])) {
        // Before an ideograph that follows something else but a space.
        const bool vrtl = (p->runs[v->run].level & 1) != 0;

        gjf[vrtl ? v->g_hi - 1 : v->g_lo] |= vrtl ? 2 : 1;
        n++;
      }
    }
  }
  return n;
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
  uint8_t * gjf = NULL;           // per glyph: the justification space it takes, before and after
  bool * hyphened = NULL;         // whether each line ends at a hyphenation point
  size_t * cand_at = NULL;        // scratch for break_lines
  GFNT_F26Dot6 * cand_fit = NULL;
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
  const bool vertical = p ? p->vertical : false;
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
  gjf = allocator->calloc_fn(allocator->ctx, p->glyph_count + 1, 1);
  hyphened = allocator->calloc_fn(allocator->ctx, p->unit_count + 2, sizeof *hyphened);
  cand_at = allocator->calloc_fn(allocator->ctx, p->unit_count + 1, sizeof *cand_at);
  cand_fit = allocator->calloc_fn(allocator->ctx, p->unit_count + 1, sizeof *cand_fit);
  if (!starts || !slices || !order || !slice_levels || !ew || !tabadv || !gjf || !hyphened
      || !cand_at || !cand_fit) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the layout");
    goto done;
  }
  line_count = break_lines(p, width, starts, hyphened, cand_at, cand_fit);
  starts[line_count] = p->unit_count;
  total_lines = line_count;
  if (p->options.max_lines && line_count > p->options.max_lines) {
    line_count = p->options.max_lines;
    more = true;
  }
  // A line's runs are its units' (at most one each) and the hyphen it may end in; the last line
  // may instead end in the truncation mark.
  runs_bytes = (p->unit_count + line_count + 2) * sizeof *runs;
  // The glyphs of the text, those of the hyphen each line but the last may end in, and the
  // truncation mark.
  total_bytes = runs_bytes
      + (p->glyph_count + line_count * p->hyphen_glyphs + ELL_GLYPHS + 1) * sizeof *placed;
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
    GFNT_F26Dot6 asc = 0, desc = 0, gap = 0, thick = 0;
    GFNT_F26Dot6 cursor = 0, lead = 0;
    bool seen_text = false;
    bool ink_set = false;
    GFNT_F26Dot6 ink_l = 0, ink_t = 0, ink_r = 0, ink_b = 0;
    PGlyph ell_g[ELL_GLYPHS];
    size_t ell_n = 0;
    GFNT_F26Dot6 ell_w = 0, natural = 0, extra = 0, extra_rem = 0;
    size_t expanded = 0;
    PRun ell_run;
    PGlyph hy_g[ELL_GLYPHS];
    size_t hy_n = 0;
    GFNT_F26Dot6 hy_w = 0;
    PRun hy_run;
    bool hy_line = false;
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
    if (hyphened[li] && ub > ua) {
      // The line ends inside a word: a hyphen follows its last letter, and takes room.
      const PRun * last = &p->runs[p->units[ub - 1].run];

      r = shape_string(p, last->span, &p->hyphen_cp[last->span], 1, hy_g, &hy_n, &hy_w,
          &hy_run, hyphen_orientation(p), error);
      if (r != GFNT_OK) {
        goto done;
      }
      hy_run.level = last->level;
      hy_line = true;
      natural += hy_w;
    }
    if (p->options.max_lines && p->ell_count && li + 1 == line_count
        && (more || (bounded && natural > width))) {
      size_t span = ub > ua ? p->runs[p->units[ub - 1].run].span
                            : (ua < p->unit_count ? p->runs[p->units[ua].run].span
                                                  : 0);

      r = shape_string(p, span, p->ell, p->ell_count, ell_g, &ell_n, &ell_w,
          &ell_run, -1, error);
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
      hy_line = false;   // the mark takes the hyphen's place
      any_truncated = true;
    }
    if (more && li + 1 == line_count) {
      any_truncated = true;
    }
    if (p->options.align == GFNT_ALIGN_JUSTIFY && bounded && !trunc_line
        && li + 1 < total_lines && ub > ua && !p->units[ub - 1].hard) {
      const size_t opportunities = mark_justification(p, ua, hang_from, gjf);
      GFNT_F26Dot6 slack = width - natural;

      if (opportunities && slack > 0) {
        extra = slack / (GFNT_F26Dot6)opportunities;
        extra_rem = slack % (GFNT_F26Dot6)opportunities;
        line->justified = true;
      }
      else {
        for (size_t j = ua; j < hang_from; j++) {
          for (size_t g = p->units[j].g_lo; g < p->units[j].g_hi; g++) {
            gjf[g] = 0;
          }
        }
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
      if (pr->ascent + pr->descent + pr->gap > thick) thick = pr->ascent + pr->descent + pr->gap;
      if (!s || s->pr != pr || s->level != level || s->hang != hang) {
        s = &slices[ns++];
        s->pr = pr;
        s->src = p->glyphs;
        s->ellipsis = false;
        s->hyphen = false;
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
    if (hy_line) {
      const size_t at = p->cp_byte[p->units[ub - 1].cp_end];
      Slice * s = &slices[ns++];

      for (size_t g = 0; g < hy_n; g++) {
        hy_g[g].cluster = (uint32_t)at;
      }
      s->pr = &hy_run;
      s->src = hy_g;
      s->ellipsis = false;
      s->hyphen = true;
      s->level = hy_run.level;
      s->hang = false;
      s->g_lo = 0;
      s->g_hi = hy_n;
      s->b_lo = s->b_hi = at;
      s->advance = 0;
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
      s->hyphen = false;
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
      thick = asc + desc + gap;
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
    if (vertical) {
      // A column is as thick as its thickest box: every box is centred on the same axis.
      line->box.x = y;   // the block offset so far; made absolute below
      line->box.width = thick;
      line->ascent = line->descent = thick / 2;
    }
    else {
      line->box.y = y;
      line->box.height = asc + desc + gap;
      line->ascent = asc;
      line->descent = desc;
      line->baseline = y + gap / 2 + asc;
    }
    for (size_t v = 0; v < ns; v++) {
      const Slice * s = &slices[order[v]];
      const PRun * pr = s->pr;
      GFNT_LineRun * lr = &runs[run_used++];

      lr->face = pr->face;
      lr->span = pr->span;
      lr->face_index = pr->face_index;
      lr->fallback = pr->face_index != 0;
      lr->ellipsis = s->ellipsis;
      lr->hyphen = s->hyphen;
      lr->level = s->level;
      lr->orientation = (GFNT_Orientation)pr->orient;
      lr->glyphs = &placed[glyph_used];
      lr->glyph_count = s->g_hi - s->g_lo;
      lr->pos = cursor;
      for (size_t g = s->g_lo; g < s->g_hi; g++) {
        const PGlyph * pg = &s->src[g];
        GFNT_PlacedGlyph * pl = &placed[glyph_used++];
        GFNT_F26Dot6 adv = pg->tab && s->src == p->glyphs ? tabadv[g] : pg->advance;

        GFNT_F26Dot6 lead = 0, trail = 0, org, l, t, rr, b;

        if (line->justified && !s->hang && s->src == p->glyphs) {
          // The space a justified line gives this glyph, before it and after it as placed.
          if (gjf[g] & 1) {
            lead = extra + (expanded < (size_t)extra_rem ? 1 : 0);
            expanded++;
          }
          if (gjf[g] & 2) {
            trail = extra + (expanded < (size_t)extra_rem ? 1 : 0);
            expanded++;
          }
        }
        org = cursor + lead;
        pl->glyph = pg->glyph;
        pl->cluster = pg->cluster;
        pl->advance = lead + adv + trail;
        if (!vertical) {
          pl->pen_x = cursor;
          pl->pen_y = line->baseline;
          pl->x = org + pg->dx;
          pl->y = line->baseline - pg->dy;
          l = pl->x + pg->ix0; t = pl->y - pg->iy1;
          rr = pl->x + pg->ix1; b = pl->y - pg->iy0;
        }
        else if (pr->orient == GFNT_ORIENT_RUN_UPRIGHT) {
          // On the axis (x = 0 here; the column's own axis is added below): the shaper's offset
          // centres the glyph's advance on it.
          pl->pen_x = 0;
          pl->pen_y = cursor;
          pl->x = pg->dx;
          pl->y = org - pg->dy;
          l = pl->x + pg->ix0; t = pl->y - pg->iy1;
          rr = pl->x + pg->ix1; b = pl->y - pg->iy0;
        }
        else {
          // Turned a quarter clockwise: the glyph's up is the page's right and its advance is
          // down. The alphabetic baseline lies half of (ascent - descent) before the axis, so the
          // em box is centred on it.
          pl->pen_x = -(pr->ascent - pr->descent) / 2;
          pl->pen_y = cursor;
          pl->x = pl->pen_x + pg->dy;
          pl->y = org + pg->dx;
          l = pl->x + pg->iy0; t = pl->y + pg->ix0;
          rr = pl->x + pg->iy1; b = pl->y + pg->ix1;
        }
        cursor += lead + adv + trail;
        if (pg->has_ink) {

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
      lr->advance = cursor - lr->pos;
      lr->start = s->b_lo;
      lr->end = s->b_hi;
      if (s->hang && !seen_text) {
        lead += lr->advance;
      }
      if (!s->hang) {
        seen_text = true;
        line->extent += lr->advance;
      }
    }
    line->truncated = trunc_line;
    if (ink_set) {
      line->ink.x = ink_l;
      line->ink.y = ink_t;
      line->ink.width = ink_r - ink_l;
      line->ink.height = ink_b - ink_t;
    }
    line->offset = -lead;   // made absolute by the alignment pass
    if (line->extent > widest) {
      widest = line->extent;
    }
    y += vertical ? line->box.width : line->box.height;
  }
  {
    GFNT_F26Dot6 avail = width == GFNT_LAYOUT_UNBOUNDED ? widest : width;
    const bool flip = p->mode == GFNT_WRITING_SIDEWAYS_LR;
    bool ink_any = false;

    for (size_t li = 0; li < line_count; li++) {
      GFNT_Line * line = &lines[li];
      GFNT_F26Dot6 slack = avail - line->extent;
      GFNT_F26Dot6 off;
      GFNT_Align a = p->options.align;
      GFNT_F26Dot6 shift;
      GFNT_F26Dot6 across = 0;   // added to what lies across the line: a column's axis

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
      shift = off + line->offset;   // line->offset held minus the leading hang
      line->offset = off;
      if (vertical) {
        // Columns stack from the right (rl) or from the left (lr); line->box.x held the
        // thickness before this one.
        const GFNT_F26Dot6 before = line->box.x;

        line->box.x = p->mode == GFNT_WRITING_VERTICAL_LR ? before : y - before - line->box.width;
        line->box.y = 0;
        line->box.height = avail;
        across = line->box.x + line->box.width / 2;
        line->baseline = across;
      }
      else {
        line->box.x = 0;
        line->box.width = avail;
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
        GFNT_LineRun * lr = (GFNT_LineRun *)&line->runs[k];

        lr->pos += shift;
        for (size_t g = 0; g < lr->glyph_count; g++) {
          GFNT_PlacedGlyph * pl = (GFNT_PlacedGlyph *)&lr->glyphs[g];

          if (vertical) {
            pl->pen_x += across;
            pl->x += across;
            pl->pen_y += shift;
            pl->y += shift;
          }
          else {
            pl->pen_x += shift;
            pl->x += shift;
          }
        }
      }
      if (flip) {
        // sideways-lr is sideways-rl turned half a turn about the box.
        const GFNT_F26Dot6 bw = y, bh = avail;

        line->box.x = bw - (line->box.x + line->box.width);
        line->offset = bh - (line->offset + line->extent);
        line->baseline = bw - line->baseline;
        if (line->ink.width || line->ink.height) {
          line->ink.x = bw - (line->ink.x + line->ink.width);
          line->ink.y = bh - (line->ink.y + line->ink.height);
        }
        for (size_t k = 0; k < line->run_count; k++) {
          GFNT_LineRun * lr = (GFNT_LineRun *)&line->runs[k];

          lr->pos = bh - (lr->pos + lr->advance);
          if (lr->orientation == GFNT_ORIENT_RUN_CW) {
            lr->orientation = GFNT_ORIENT_RUN_CCW;
          }
          for (size_t g = 0; g < lr->glyph_count; g++) {
            GFNT_PlacedGlyph * pl = (GFNT_PlacedGlyph *)&lr->glyphs[g];

            pl->pen_x = bw - pl->pen_x;
            pl->pen_y = bh - pl->pen_y;
            pl->x = bw - pl->x;
            pl->y = bh - pl->y;
          }
        }
      }
      if (line->ink.width || line->ink.height) {
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
    }
  }
  out->lines = lines;
  out->line_count = line_count;
  out->writing_mode = p->mode;
  out->width = vertical ? y : width == GFNT_LAYOUT_UNBOUNDED ? widest : width;
  out->height = vertical ? (width == GFNT_LAYOUT_UNBOUNDED ? widest : width) : y;
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
  allocator->free_fn(allocator->ctx, gjf);
  allocator->free_fn(allocator->ctx, hyphened);
  allocator->free_fn(allocator->ctx, cand_at);
  allocator->free_fn(allocator->ctx, cand_fit);
  allocator->free_fn(allocator->ctx, lines);
  allocator->free_fn(allocator->ctx, storage);
  return r;
}
