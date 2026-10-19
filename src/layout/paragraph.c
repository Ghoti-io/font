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
 * Making a paragraph: itemise, shape and keep the units.
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

static GFNT_F26Dot6 scale_v(GFNT_F26Dot6 v, GFNT_F16Dot16 scale) {
  int64_t t = (int64_t)v * scale;

  return (GFNT_F26Dot6)((t >= 0 ? t + 32768 : t - 32768) / 65536);
}

/** The outline box of a glyph in pixels, read once. Only an OOM is an error. */
GFNT_Result gfnt_lay_glyph_ink(PGlyph * pg, const GFNT_Face * face,
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

bool gfnt_lay_face_maps(const GFNT_Face * face, uint32_t cp) {
  uint32_t g = 0;

  return gfnt_face_glyph_for_codepoint(face, cp, &g, NULL) == GFNT_OK && g != 0;
}

void gfnt_lay_scale_metrics(const GFNT_Face * face, const GFNT_TextStyle * style,
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
  const GFNT_Os2 * os2 = NULL;
  if (gfnt_face_os2(face, &os2, NULL) == GFNT_OK && os2->x_height > 0) {
    run->x_height = gfnt_units_to_pixels(os2->x_height, scale);
  }
  else {
    /* Fonts without an OS/2 x-height use half the ascent as a conservative fallback. */
    run->x_height = run->ascent / 2;
  }
}

/* Prepare the per-run inline box. Arithmetic is kept in 64 bits until range checked. */
bool gfnt_lay_set_inline_metrics(PRun * run, const GFNT_TextStyle * style) {
  int64_t content = (int64_t)run->ascent + run->descent;
  int64_t natural = content + run->gap;
  int64_t height = natural;
  int64_t leading;
  int64_t shift = 0;
  int64_t inline_ascent, inline_descent;

  run->vertical_align = style->vertical_align;
  switch (style->line_height.kind) {
    case GFNT_LINE_HEIGHT_NORMAL:
      break;
    case GFNT_LINE_HEIGHT_MULTIPLE:
      height = ((int64_t)style->size * style->line_height.value) / 0x10000;
      break;
    case GFNT_LINE_HEIGHT_LENGTH:
      height = style->line_height.value;
      break;
    default:
      return false;
  }
  if (height < 0 || height > INT32_MAX || natural > INT32_MAX) return false;
  leading = height - content;
  int64_t half_leading = (leading / 2 / GFNT_F26DOT6_ONE) * GFNT_F26DOT6_ONE;
  if (half_leading < INT32_MIN || half_leading > INT32_MAX) return false;
  run->half_leading = (GFNT_F26Dot6)half_leading;
  run->line_height = (GFNT_F26Dot6)height;
  inline_ascent = (int64_t)run->ascent + run->half_leading;
  inline_descent = height - inline_ascent;
  if (inline_ascent < INT32_MIN || inline_ascent > INT32_MAX
      || inline_descent < INT32_MIN || inline_descent > INT32_MAX) return false;
  run->inline_ascent = (GFNT_F26Dot6)inline_ascent;
  run->inline_descent = (GFNT_F26Dot6)inline_descent;
  switch (style->vertical_align) {
    case GFNT_VERTICAL_ALIGN_BASELINE:
    case GFNT_VERTICAL_ALIGN_TEXT_TOP:
    case GFNT_VERTICAL_ALIGN_TEXT_BOTTOM:
    case GFNT_VERTICAL_ALIGN_MIDDLE:
    case GFNT_VERTICAL_ALIGN_TOP:
    case GFNT_VERTICAL_ALIGN_BOTTOM:
      break;
    case GFNT_VERTICAL_ALIGN_SUB:
      shift = -(int64_t)style->size / 5;
      break;
    case GFNT_VERTICAL_ALIGN_SUPER:
      shift = (int64_t)style->size / 3;
      break;
    case GFNT_VERTICAL_ALIGN_LENGTH:
      shift = style->vertical_align_value;
      break;
    case GFNT_VERTICAL_ALIGN_PERCENTAGE:
      shift = ((int64_t)height * style->vertical_align_value) / 0x10000;
      break;
    default:
      return false;
  }
  if (shift < INT32_MIN || shift > INT32_MAX
      || inline_ascent + shift < INT32_MIN || inline_ascent + shift > INT32_MAX
      || inline_descent - shift < INT32_MIN || inline_descent - shift > INT32_MAX) return false;
  run->shift = (GFNT_F26Dot6)shift;
  return true;
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

  for (size_t i = 0; i < p->annotation_count; i++) {
    gfnt_paragraph_free(p->annotations[i].shaped);
    gfnt_paragraph_free(p->annotations[i].alternate_shaped);
    allocator->free_fn(allocator->ctx, p->annotations[i].text);
  }

  allocator->free_fn(allocator->ctx, p->cp_byte);
  allocator->free_fn(allocator->ctx, p->cps);
  if (p->owned_features) {
    for (size_t i = 0; i < p->span_count; i++) {
      allocator->free_fn(allocator->ctx, p->owned_features[i]);
    }
  }
  allocator->free_fn(allocator->ctx, p->owned_features);
  allocator->free_fn(allocator->ctx, p->styles);
  allocator->free_fn(allocator->ctx, p->ideographic_advance);
  allocator->free_fn(allocator->ctx, p->runs);
  allocator->free_fn(allocator->ctx, p->glyphs);
  allocator->free_fn(allocator->ctx, p->units);
  allocator->free_fn(allocator->ctx, p->annotations);
  allocator->free_fn(allocator->ctx, p->hyphen_width);
  allocator->free_fn(allocator->ctx, p->hyphen_cp);
  allocator->free_fn(allocator->ctx, p->kpoints);
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

    GFNT_TextStyle * style = &p->styles[p->runs[run].span];

    if (u->g_hi > u->g_lo) {
      for (size_t g = u->g_lo; g < u->g_hi; g++) p->glyphs[g].unit = i;
      const int64_t extra = (int64_t)style->letter_spacing
          + (u->cp_end == u->cp_start + 1 && cps[u->cp_start] == 0x20
              ? style->word_spacing : 0);
      PGlyph * last = &p->glyphs[rtl ? u->g_lo : u->g_hi - 1];
      const int64_t advance = (int64_t)last->advance + extra;

      if (advance < INT32_MIN || advance > INT32_MAX) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, last->glyph,
            "letter or word spacing overflows a glyph advance");
      }
      last->advance = (GFNT_F26Dot6)advance;
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
    u->base_width = u->width;
    if (!p->ideographic_advance[p->runs[run].span]
        && gfnt_lay_text_spacing_ideograph(gfnt_lay_autospace_base_cp(p, u))) {
      int64_t advance = (int64_t)u->base_width - style->letter_spacing;
      if (advance > 0 && advance <= INT32_MAX) {
        p->ideographic_advance[p->runs[run].span] = (GFNT_F26Dot6)advance;
      }
    }
    for (size_t c = u->cp_start; c < u->cp_end; c++) {
      all_space = all_space && gfnt_lay_hangs(cps[c]);
    }
    u->space = all_space;
    if (all_space && u->g_hi > u->g_lo) {
      p->glyphs[u->g_lo].expand = true;
    }
  }
  (void)error;
  return GFNT_OK;
}

/** Everything the stages of gfnt_paragraph_create() share. */
typedef struct {
  const char * text;
  size_t length;
  const GFNT_TextSpan * spans;
  size_t span_count;
  const GFNT_ParagraphOptions * options;
  const GFNT_Allocator * allocator;
  GFNT_Error * error;
  GFNT_Paragraph * p;
  uint32_t * cps;
  uint8_t * levels;
  uint8_t * ori;               // per code point, in vertical text: ORI_UPRIGHT or ORI_SIDEWAYS
  size_t * span_cp;            // span_count + 1 code point starts
  size_t * source_span_cp;
  size_t * source_to_layout;
  uint32_t * transformed_cps;
  size_t * transformed_bytes;
  size_t * transformed_spans;
  bool * edge;                 // item boundaries
  bool * opp;
  bool * emergency_opp;
  bool * collapsed;
  bool * removed_break;
  bool emergency_breaks;
  size_t * breaks;
  GUNI_ScriptItem * scripts;
  size_t n;
  bool cps_owned;              // the paragraph holds cps, and frees it
  size_t total_runs;
} CreateState;

/** Refuse an option, a style or a span list the paragraph cannot be made from. */
static GFNT_Result create_validate(const char * text, size_t length, const GFNT_TextSpan * spans,
    size_t span_count, const GFNT_ParagraphOptions * options, GFNT_Error * error) {
  (void)text;
  if ((unsigned)options->direction > GFNT_BIDI_AUTO
      || (unsigned)options->align > GFNT_ALIGN_JUSTIFY
      || (unsigned)options->align_last > GFNT_ALIGN_LAST_JUSTIFY
      || (unsigned)options->overflow_wrap > GFNT_OVERFLOW_WRAP_BREAK_WORD
      || (unsigned)options->word_break > GFNT_WORD_BREAK_AUTO_PHRASE
      || (unsigned)options->white_space > GFNT_WHITE_SPACE_PRE_LINE
      || (unsigned)options->line_break > GFNT_LINE_BREAK_ANYWHERE
      || (unsigned)options->hyphens > GFNT_HYPHENS_MANUAL_PANGO
      || (unsigned)options->text_wrap_style > GFNT_TEXT_WRAP_AVOID_SHORT_LAST_LINE
      || (unsigned)options->text_autospace > GFNT_TEXT_AUTOSPACE_NUMERIC
      || (unsigned)options->text_spacing_trim > GFNT_TEXT_SPACING_TRIM_TRIM_START
      || (unsigned)options->justify > GFNT_JUSTIFY_NONE
      || options->space_share < 0 || options->space_share > 0x10000
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
      if (s->feature_count && !s->features) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "a span has shaping features without a feature array");
      }
      if ((unsigned)s->line_height.kind > GFNT_LINE_HEIGHT_LENGTH
          || (s->line_height.kind == GFNT_LINE_HEIGHT_MULTIPLE && s->line_height.value < 0)
          || (s->line_height.kind == GFNT_LINE_HEIGHT_LENGTH && s->line_height.value < 0)
          || (unsigned)s->vertical_align > GFNT_VERTICAL_ALIGN_BOTTOM
          || (unsigned)s->content_language > GFNT_CONTENT_LANGUAGE_JAPANESE
          || ((unsigned)s->text_transform & ~0x0fu) != 0
          || (unsigned)s->case_tailoring > GFNT_CASE_TAILORING_LITHUANIAN
          || (((unsigned)s->text_transform & GFNT_TEXT_TRANSFORM_CASE_MASK) == GFNT_TEXT_TRANSFORM_NONE
              && s->case_tailoring != GFNT_CASE_TAILORING_NONE)) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "a span has an invalid line-height, vertical alignment, or text transform");
      }
      if (s->inline_object) {
        const GFNT_InlineObject * object = s->inline_object;
        if (object->advance < 0 || object->ascent < 0 || object->descent < 0
            || (int64_t)object->ascent + object->descent > INT32_MAX
            || (unsigned)object->align > GFNT_VERTICAL_ALIGN_BOTTOM) {
          return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
              "an inline object has invalid dimensions or alignment");
        }
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
  return GFNT_OK;
}

/** Allocate the paragraph and its per-code-point and per-span arrays. */
static GFNT_Result create_allocate(CreateState * cs) {
  const size_t length = cs->length;
  const size_t span_count = cs->span_count;
  const GFNT_ParagraphOptions * options = cs->options;
  const GFNT_Allocator * allocator = cs->allocator;
  GFNT_Error * error = cs->error;
  GFNT_Paragraph * p = cs->p;
  GFNT_Result r = GFNT_OK;

  p = cs->p = allocator->calloc_fn(allocator->ctx, 1, sizeof *p);
  cs->cps = ALLOC(cs->cps, length);
  cs->span_cp = ALLOC(cs->span_cp, span_count + 1);
  if (p) {
    p->allocator = allocator;
  }
  if (!p || !cs->cps || !cs->span_cp) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the paragraph");
    return r;
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
  p->ideographic_advance = allocator->calloc_fn(allocator->ctx, span_count,
      sizeof *p->ideographic_advance);
  p->owned_features = allocator->calloc_fn(allocator->ctx, span_count,
      sizeof *p->owned_features);
  if (!p->cp_byte || !p->styles || !p->ideographic_advance || !p->owned_features) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the paragraph");
    return r;
  }
  return r;
}

/** Decode the UTF-8 text and note the code point at which every span starts. */
static GFNT_Result create_decode(CreateState * cs) {
  const char * text = cs->text;
  const size_t length = cs->length;
  const GFNT_TextSpan * spans = cs->spans;
  const size_t span_count = cs->span_count;
  GFNT_Error * error = cs->error;
  GFNT_Paragraph * p = cs->p;
  GFNT_Result r = GFNT_OK;

  {
    // Decode, noting the code point at which every span starts.
    size_t pos = 0;
    size_t next_span = 0;
    size_t boundary = 0;   // the byte at which span next_span starts

    for (size_t s = 0; s <= span_count; s++) {
      cs->span_cp[s] = (size_t)-1;
    }
    while (pos < length) {
      uint32_t cp;
      size_t used;

      while (next_span < span_count && boundary == pos) {
        cs->span_cp[next_span] = cs->n;
        boundary += spans[next_span].length;
        next_span++;
      }
      if (boundary < pos) {
        r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, pos, GFNT_GLYPH_NONE,
            "a span ends inside a character");
        return r;
      }
      used = guni_utf8_decode(text + pos, length - pos, &cp, NULL);
      p->cp_byte[cs->n] = pos;
      cs->cps[cs->n++] = cp;
      pos += used;
    }
    while (next_span < span_count && boundary == pos) {
      cs->span_cp[next_span] = cs->n;
      boundary += spans[next_span].length;
      next_span++;
    }
    if (next_span < span_count) {
      r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, pos, GFNT_GLYPH_NONE,
          "a span ends inside a character");
      return r;
    }
    p->cp_byte[cs->n] = length;
    cs->span_cp[span_count] = cs->n;
  }
  return r;
}

/** Apply text-transform, then check the annotation ranges and the inline-object spans. */
static GFNT_Result create_transform(CreateState * cs) {
  const size_t length = cs->length;
  const GFNT_TextSpan * spans = cs->spans;
  const size_t span_count = cs->span_count;
  const GFNT_ParagraphOptions * options = cs->options;
  const GFNT_Allocator * allocator = cs->allocator;
  GFNT_Error * error = cs->error;
  GFNT_Paragraph * p = cs->p;
  GFNT_Result r = GFNT_OK;

  cs->source_span_cp = ALLOC(cs->source_span_cp, span_count + 1);
  if (!cs->source_span_cp) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for source span boundaries");
    return r;
  }
  memcpy(cs->source_span_cp, cs->span_cp, (span_count + 1) * sizeof *cs->source_span_cp);
  {
    bool needs_transform = false;
    for (size_t s = 0; s < span_count; s++) {
      needs_transform = needs_transform
          || spans[s].style.text_transform != GFNT_TEXT_TRANSFORM_NONE;
    }
    if (needs_transform && cs->n) {
      size_t transformed_length = 0;
      r = gfnt_lay_transform_case(cs->cps, cs->n, p->cp_byte, spans, span_count, cs->span_cp, options->white_space, allocator,
          &cs->transformed_cps, &cs->transformed_bytes, &transformed_length, &cs->transformed_spans,
          &cs->source_to_layout);
      if (r != GFNT_OK) {
        r = gfnt_error_set(error, r, 0, 0, GFNT_GLYPH_NONE,
            r == GFNT_ERR_OOM ? "no memory for transformed text" : "text transform failed");
        return r;
      }
      allocator->free_fn(allocator->ctx, cs->cps);
      allocator->free_fn(allocator->ctx, p->cp_byte);
      allocator->free_fn(allocator->ctx, cs->span_cp);
      cs->cps = cs->transformed_cps;
      p->cp_byte = cs->transformed_bytes;
      cs->span_cp = cs->transformed_spans;
      cs->transformed_cps = NULL;
      cs->transformed_bytes = NULL;
      cs->transformed_spans = NULL;
      cs->n = transformed_length;
    }
  }
  if (!gfnt_lay_annotation_tree_valid(options->annotations, options->annotation_count,
          length, 0, length, 0)) {
    r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "annotation ranges or descriptors are invalid");
    return r;
  }
  p->length = cs->n;
  p->cps = cs->cps;
  cs->cps_owned = true;
  p->span_count = span_count;
  for (size_t s = 0; s < span_count; s++) {
    if (spans[s].style.inline_object) {
      if (spans[s].length != 3 || cs->span_cp[s + 1] != cs->span_cp[s] + 1
          || cs->span_cp[s] >= cs->n || cs->cps[cs->span_cp[s]] != 0xFFFC) {
        r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "an inline object descriptor must cover exactly one U+FFFC");
        return r;
      }
    }
  }
  return r;
}

/** Mark the whitespace that white-space collapsing removes. */
static GFNT_Result create_collapse(CreateState * cs) {
  const GFNT_ParagraphOptions * options = cs->options;
  const GFNT_Allocator * allocator = cs->allocator;
  GFNT_Error * error = cs->error;
  GFNT_Result r = GFNT_OK;

  if (options->white_space == GFNT_WHITE_SPACE_NORMAL
      || options->white_space == GFNT_WHITE_SPACE_NOWRAP
      || options->white_space == GFNT_WHITE_SPACE_PRE_LINE) {
    cs->collapsed = ALLOC(cs->collapsed, cs->n);
    cs->removed_break = ALLOC(cs->removed_break, cs->n);
    if (!cs->collapsed || !cs->removed_break) {
      r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory for cs->collapsed whitespace flags");
      return r;
    }
    for (size_t i = 0; i < cs->n;) {
      size_t end;
      bool has_segment_break = false;
      bool discard = false;
      const bool pre_line = options->white_space == GFNT_WHITE_SPACE_PRE_LINE;

      if (pre_line && gfnt_lay_css_segment_break(cs->cps[i])) {
        i++;
        continue;
      }
      if (!gfnt_lay_css_collapsible_space(cs->cps[i])) {
        i++;
        continue;
      }
      end = i;
      while (end < cs->n && gfnt_lay_css_collapsible_space(cs->cps[end])
          && !(pre_line && gfnt_lay_css_segment_break(cs->cps[end]))) {
        has_segment_break = has_segment_break || gfnt_lay_css_segment_break(cs->cps[end]);
        end++;
      }
      discard = i == 0 || (pre_line && i > 0 && gfnt_lay_css_segment_break(cs->cps[i - 1]));
      if (!pre_line && has_segment_break && i > 0 && end < cs->n
          && gfnt_lay_is_ideographic(cs->cps[i - 1]) && gfnt_lay_is_ideographic(cs->cps[end])) {
        discard = true;
        cs->removed_break[i - 1] = options->line_break == GFNT_LINE_BREAK_ANYWHERE
            || options->word_break != GFNT_WORD_BREAK_KEEP_ALL
            || !gfnt_lay_keep_all_pair(cs->cps[i - 1], cs->cps[end],
                options->line_break == GFNT_LINE_BREAK_STRICT
                    ? GUNI_LINE_BREAK_STRICT : GUNI_LINE_BREAK_NORMAL);
      }
      for (size_t c = i; c < end; c++) {
        cs->cps[c] = 0x20;
        cs->collapsed[c] = discard || c != i;
      }
      i = end;
    }
  }
  return r;
}

/** Copy the span styles, mapping shaping-feature ranges across a text transform. */
static GFNT_Result create_styles(CreateState * cs) {
  const GFNT_TextSpan * spans = cs->spans;
  const size_t span_count = cs->span_count;
  const GFNT_Allocator * allocator = cs->allocator;
  GFNT_Error * error = cs->error;
  GFNT_Paragraph * p = cs->p;
  GFNT_Result r = GFNT_OK;

  for (size_t i = 0; i < span_count; i++) {
    p->styles[i] = spans[i].style;
    if (cs->source_to_layout && spans[i].style.feature_count) {
      const size_t source_start = cs->source_span_cp[i];
      const size_t source_length = cs->source_span_cp[i + 1] - source_start;
      const size_t layout_start = cs->span_cp[i];
      p->owned_features[i] = ALLOC(p->owned_features[i], spans[i].style.feature_count);
      if (!p->owned_features[i]) {
        r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for transformed shaping features");
        return r;
      }
      memcpy(p->owned_features[i], spans[i].style.features,
          spans[i].style.feature_count * sizeof *p->owned_features[i]);
      for (size_t f = 0; f < spans[i].style.feature_count; f++) {
        GFNT_ShapeFeature * feature = &p->owned_features[i][f];
        size_t start = feature->start > source_length ? source_length : feature->start;
        size_t end = feature->end == GFNT_SHAPE_END ? source_length
            : (feature->end > source_length ? source_length : feature->end);
        feature->start = cs->source_to_layout[source_start + start] - layout_start;
        if (feature->end != GFNT_SHAPE_END) {
          feature->end = cs->source_to_layout[source_start + end] - layout_start;
        }
      }
      p->styles[i].features = p->owned_features[i];
    }
  }
  return r;
}

/** Bidirectional levels, vertical orientation, item edges and break opportunities. */
static GFNT_Result create_analyse(CreateState * cs) {
  const GFNT_TextSpan * spans = cs->spans;
  const size_t span_count = cs->span_count;
  const GFNT_ParagraphOptions * options = cs->options;
  const GFNT_Allocator * allocator = cs->allocator;
  GFNT_Error * error = cs->error;
  GFNT_Paragraph * p = cs->p;
  GFNT_Result r = GFNT_OK;

  cs->levels = ALLOC(cs->levels, cs->n);
  cs->ori = ALLOC(cs->ori, cs->n);
  cs->edge = ALLOC(cs->edge, cs->n + 1);
  cs->opp = ALLOC(cs->opp, cs->n + 1);
  if (cs->emergency_breaks) {
    cs->emergency_opp = ALLOC(cs->emergency_opp, cs->n + 1);
  }
  if (!cs->levels || !cs->ori || !cs->edge || !cs->opp
      || (cs->emergency_breaks && !cs->emergency_opp)) {
    r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the paragraph");
    return r;
  }
  if (cs->n) {
    GUNI_BidiDirection dir = options->direction == GFNT_BIDI_RTL ? GUNI_BIDI_RTL
        : options->direction == GFNT_BIDI_AUTO ? GUNI_BIDI_AUTO : GUNI_BIDI_LTR;
    size_t script_count = 0;
    GUNI_BreakOptions bo;
    size_t break_count = 0;

    if (guni_bidi_levels_with_allocator(cs->cps, cs->n, dir, NULL, cs->levels, cs->n,
            &p->para_level, NULL) != GUNI_OK) {
      r = gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
          "the text is too long for the bidirectional algorithm");
      return r;
    }
    if (p->vertical && options->text_orientation == GFNT_ORIENT_UPRIGHT
        && p->mode != GFNT_WRITING_SIDEWAYS_RL && p->mode != GFNT_WRITING_SIDEWAYS_LR) {
      // Upright text reads as left to right whatever its characters are (CSS Writing Modes).
      memset(cs->levels, 0, cs->n);
      p->para_level = 0;
    }
    if (p->vertical) {
      size_t sp = 0;

      for (size_t i = 0; i < cs->n; i++) {
        while (sp + 1 < span_count && cs->span_cp[sp + 1] <= i) {
          sp++;
        }
        if (i > 0 && gfnt_lay_inherits_orientation(cs->cps[i])) {
          cs->ori[i] = cs->ori[i - 1];
        }
        else if (options->text_orientation == GFNT_ORIENT_MIXED_PANGO
            && (cs->cps[i] == 0x261D || (cs->cps[i] >= 0x2795 && cs->cps[i] <= 0x2797) || cs->cps[i] == 0x27B0
                || cs->cps[i] == 0x27BF)) {
          // Pango sets these emoji-like symbols with the text they follow.
          cs->ori[i] = i > 0 ? cs->ori[i - 1] : ORI_SIDEWAYS;
        }
        else {
          cs->ori[i] = gfnt_lay_char_orientation(p, &spans[sp].style, cs->cps[i]);
        }
      }
    }
    // A call with no room reports the count it needs, with a limit error.
    (void)guni_script_items(cs->cps, cs->n, NULL, 0, &script_count);
    if (script_count) {
      cs->scripts = ALLOC(cs->scripts, script_count);
      if (!cs->scripts) {
        r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for the paragraph");
        return r;
      }
      if (guni_script_items(cs->cps, cs->n, cs->scripts, script_count, &script_count)
          == GUNI_OK) {
        for (size_t i = 0; i < script_count; i++) {
          cs->edge[cs->scripts[i].start] = true;
        }
      }
    }
    for (size_t s = 0; s < span_count; s++) {
      if (cs->span_cp[s] < cs->n) {
        cs->edge[cs->span_cp[s]] = true;
      }
    }
    for (size_t i = 1; i < cs->n; i++) {
      if (cs->levels[i] != cs->levels[i - 1] || (p->vertical && cs->ori[i] != cs->ori[i - 1])) {
        cs->edge[i] = true;
      }
    }
    cs->edge[0] = true;
    cs->edge[cs->n] = true;

    memset(&bo, 0, sizeof bo);
    bo.kind = GUNI_BREAK_LINE;
    bo.tailoring = options->line_break == GFNT_LINE_BREAK_STRICT
        ? GUNI_LINE_BREAK_STRICT
        : options->line_break == GFNT_LINE_BREAK_LOOSE ? GUNI_LINE_BREAK_LOOSE
        : options->line_break == GFNT_LINE_BREAK_ANYWHERE
            ? GUNI_LINE_BREAK_ANYWHERE : GUNI_LINE_BREAK_NORMAL;
    (void)guni_break_all_codepoints(&bo, cs->cps, cs->n, NULL, 0, &break_count);
    if (break_count) {
      cs->breaks = ALLOC(cs->breaks, break_count);
      if (!cs->breaks) {
        r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for the paragraph");
        return r;
      }
      if (guni_break_all_codepoints(&bo, cs->cps, cs->n, cs->breaks, break_count,
              &break_count) == GUNI_OK) {
        for (size_t i = 0; i < break_count; i++) {
          if (cs->breaks[i] <= cs->n
              && !(cs->breaks[i] > 0 && cs->breaks[i] < cs->n
                  && p->cp_byte[cs->breaks[i]] == p->cp_byte[cs->breaks[i] - 1])) {
            cs->opp[cs->breaks[i]] = true;
          }
        }
      }
    }
    if (cs->emergency_breaks) {
      bo.kind = GUNI_BREAK_GRAPHEME;
      (void)guni_break_all_codepoints(&bo, cs->cps, cs->n, NULL, 0, &break_count);
      allocator->free_fn(allocator->ctx, cs->breaks);
      cs->breaks = NULL;
      if (break_count) {
        cs->breaks = ALLOC(cs->breaks, break_count);
        if (!cs->breaks) {
          r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
              "no memory for the paragraph");
          return r;
        }
        if (guni_break_all_codepoints(&bo, cs->cps, cs->n, cs->breaks, break_count,
                &break_count) == GUNI_OK) {
          for (size_t i = 0; i < break_count; i++) {
            if (cs->breaks[i] > 0 && cs->breaks[i] < cs->n
                && p->cp_byte[cs->breaks[i]] != p->cp_byte[cs->breaks[i] - 1]) {
              cs->emergency_opp[cs->breaks[i]] = true;
            }
          }
        }
      }
    }
  }
  return r;
}

/** Shape one U+FFFC that carries an inline-object descriptor: one run, one glyph, one unit. */
static GFNT_Result create_shape_object(CreateState * cs, size_t start, size_t end, size_t span,
    size_t * glyph_cap) {
  GFNT_Paragraph * p = cs->p;
  const GFNT_Allocator * allocator = cs->allocator;
  GFNT_Error * error = cs->error;
  const GFNT_TextStyle * style = &cs->spans[span].style;
  GFNT_Result r = GFNT_OK;

  const GFNT_InlineObject * object = style->inline_object;
  GFNT_TextStyle object_style = *style;
  PRun * run = &p->runs[p->run_count];
  PGlyph * glyph;
  Unit * unit;

  if (p->glyph_count == *glyph_cap) {
    size_t cap = *glyph_cap ? *glyph_cap * 2 : 64;
    PGlyph * bigger;
    while (cap <= p->glyph_count) cap *= 2;
    bigger = allocator->realloc_fn(allocator->ctx, p->glyphs, cap * sizeof *bigger);
    if (!bigger) {
      r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory for the inline object glyph");
      return r;
    }
    p->glyphs = bigger;
    *glyph_cap = cap;
  }
  memset(run, 0, sizeof *run);
  run->span = span;
  run->level = cs->levels[start];
  run->orient = p->vertical ? GFNT_ORIENT_RUN_UPRIGHT : GFNT_ORIENT_RUN_HORIZONTAL;
  run->ascent = object->ascent;
  run->descent = object->descent;
  run->inline_ascent = object->ascent;
  run->inline_descent = object->descent;
  run->line_height = object->ascent + object->descent;
  run->object = object;
  object_style.vertical_align = object->align;
  object_style.vertical_align_value = object->align_value;
  if (!gfnt_lay_set_inline_metrics(run, &object_style)) {
    r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, p->cp_byte[start], GFNT_GLYPH_NONE,
        "an inline object has invalid computed line metrics");
    return r;
  }
  glyph = &p->glyphs[p->glyph_count];
  memset(glyph, 0, sizeof *glyph);
  glyph->glyph = GFNT_GLYPH_NONE;
  glyph->cluster = (uint32_t)p->cp_byte[start];
  glyph->advance = object->advance;
  glyph->has_ink = false;
  unit = &p->units[p->unit_count++];
  memset(unit, 0, sizeof *unit);
  unit->cp_start = start;
  unit->cp_end = end;
  unit->run = p->run_count;
  unit->g_lo = p->glyph_count;
  unit->g_hi = p->glyph_count + 1;
  unit->object = true;
  if ((int64_t)object->advance + style->letter_spacing < INT32_MIN
      || (int64_t)object->advance + style->letter_spacing > INT32_MAX) {
    r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, p->cp_byte[start], GFNT_GLYPH_NONE,
        "an inline object advance exceeds the supported layout range");
    return r;
  }
  unit->width = (GFNT_F26Dot6)((int64_t)object->advance + style->letter_spacing);
  unit->base_width = unit->width;
  p->glyphs[p->glyph_count].advance = unit->width;
  p->glyph_count++;
  p->run_count++;
  return r;
}

/** Shape every item into runs, glyphs and units. */
static GFNT_Result create_shape(CreateState * cs) {
  const GFNT_TextSpan * spans = cs->spans;
  const size_t span_count = cs->span_count;
  const GFNT_Allocator * allocator = cs->allocator;
  GFNT_Error * error = cs->error;
  GFNT_Paragraph * p = cs->p;
  GFNT_Result r = GFNT_OK;

  if (cs->n) {
    // Count the face runs the items will give, so the arrays can be sized: an
    // item shapes into at most one run per code point.
    cs->total_runs = cs->n;
    p->runs = ALLOC(p->runs, cs->total_runs);
    p->units = ALLOC(p->units, cs->n + 1);
    if (!p->runs || !p->units) {
      r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
          "no memory for the paragraph");
      return r;
    }
  }
  {
    // The glyph array grows as items are shaped.
    size_t glyph_cap = 0;
    size_t span = 0;

    for (size_t start = 0; start < cs->n;) {
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

      while (end < cs->n && !cs->edge[end]) {
        end++;
      }
      while (span + 1 < span_count && cs->span_cp[span + 1] <= start) {
        span++;
      }
      style = &spans[span].style;
      if (end == start + 1 && cs->cps[start] == 0xFFFC && style->inline_object) {
        r = create_shape_object(cs, start, end, span, &glyph_cap);
        if (r != GFNT_OK) {
          return r;
        }
        start = end;
        continue;
      }
      flist = style->faces;
      fcount = style->face_count;
      upright = p->vertical && cs->ori[start] == ORI_UPRIGHT;
      rtl = !upright && (cs->levels[start] & 1) != 0;
      rev = upright && (cs->levels[start] & 1) != 0;
      memset(&so, 0, sizeof so);
      so.language = style->language;
      so.direction = upright ? GFNT_DIRECTION_TTB
                             : rtl ? GFNT_DIRECTION_RTL : GFNT_DIRECTION_LTR;
      so.variation = style->variation;
      features = cut_features(allocator, style, cs->span_cp[span], start, end,
          &feature_count);
      if (style->feature_count && !features) {
        r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
            "no memory for the features");
        return r;
      }
      so.features = features;
      so.feature_count = feature_count;
      memset(&fr, 0, sizeof fr);
      if (p->provider.font_for) {
        GFNT_Tag item_script = gfnt_shape_script_of(cs->cps + start, end - start);

        owned = allocator->calloc_fn(allocator->ctx,
            style->face_count + PROVIDER_MAX, sizeof *owned);
        if (!owned) {
          allocator->free_fn(allocator->ctx, features);
          r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
              "no memory for the face list");
          return r;
        }
        memcpy(owned, style->faces, style->face_count * sizeof *owned);
        flist = owned;
        fcount = style->face_count;
        for (size_t c = start; c < end; c++) {
          bool mapped = false;
          const GFNT_Face * f;

          if (provider_skips(cs->cps[c])) {
            continue;
          }
          for (size_t k = 0; k < fcount && !mapped; k++) {
            mapped = gfnt_lay_face_maps(owned[k], cs->cps[c]);
          }
          if (mapped) {
            continue;
          }
          f = p->provider.font_for(p->provider.ctx, cs->cps[c], item_script,
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
      r = gfnt_faces_shape(flist, fcount, cs->cps + start, end - start, &so,
          allocator, &fr, error);
      allocator->free_fn(allocator->ctx, features);
      if (r != GFNT_OK) {
        allocator->free_fn(allocator->ctx, owned);
        owned = NULL;
        return r;
      }
      // The face runs come back in visual order; take them in logical order.
      for (size_t k = 0; k < fr.count && r == GFNT_OK; k++) {
        const GFNT_FaceRun * f = &fr.runs[rtl ? fr.count - 1 - k : k];
        GFNT_F16Dot16 scale;
        PRun * pr;

        if (p->run_count >= cs->total_runs) {
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
        pr->level = cs->levels[start];
        pr->orient = p->vertical ? (upright ? GFNT_ORIENT_RUN_UPRIGHT : GFNT_ORIENT_RUN_CW)
                                 : GFNT_ORIENT_RUN_HORIZONTAL;
        gfnt_lay_scale_metrics(pr->face, style, pr);
        if (!gfnt_lay_set_inline_metrics(pr, style)) {
          r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
              "a span has an invalid line-height or vertical alignment");
          break;
        }
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
          pg->cluster = (uint32_t)p->cp_byte[cp < cs->n ? cp : cs->n];
          pg->advance = gfnt_units_to_pixels(upright ? -sg->y_advance : sg->x_advance, scale);
          pg->dx = gfnt_units_to_pixels(sg->x_offset, scale);
          pg->dy = gfnt_units_to_pixels(sg->y_offset, scale);
          pg->expand = false;
          pg->tab = false;
          if (r == GFNT_OK) {
            r = gfnt_lay_glyph_ink(pg, pr->face, style, scale, allocator);
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
              p->glyph_count, cs->cps, error);

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
        return r;
      }
      start = end;
    }
  }
  return r;
}

/** Put the break opportunities, collapsed spaces and hard breaks onto the units. */
static GFNT_Result create_units(CreateState * cs) {
  const GFNT_ParagraphOptions * options = cs->options;
  GFNT_Paragraph * p = cs->p;
  GFNT_Result r = GFNT_OK;

  {
    // Where a line may and must break, onto the units.
    for (size_t i = 0; i < p->unit_count; i++) {
      Unit * u = &p->units[i];
      uint32_t last = cs->cps[u->cp_end - 1];

      u->opportunity = cs->opp[u->cp_end];
      u->emergency = cs->emergency_opp && cs->emergency_opp[u->cp_end];
      if (cs->collapsed && u->cp_start < cs->n && cs->collapsed[u->cp_start]) {
        bool all_collapsed = true;

        for (size_t c = u->cp_start; c < u->cp_end; c++) {
          all_collapsed = all_collapsed && cs->collapsed[c];
        }
        if (all_collapsed) {
          u->width = 0;
          u->base_width = 0;
          u->tab = false;
          u->opportunity = false;
          u->emergency = false;
          for (size_t g = u->g_lo; g < u->g_hi; g++) {
            p->glyphs[g].advance = 0;
            p->glyphs[g].tab = false;
          }
        }
      }
      if (cs->collapsed && u->space && u->cp_start < cs->n && !cs->collapsed[u->cp_start]) {
        u->opportunity = true;
      }
      if (cs->removed_break && u->cp_end && cs->removed_break[u->cp_end - 1]) {
        u->opportunity = true;
      }
      if (options->white_space == GFNT_WHITE_SPACE_BREAK_SPACES && u->space) {
        u->opportunity = true;
      }
      if (options->word_break == GFNT_WORD_BREAK_BREAK_ALL && u->emergency
          && u->cp_end < cs->n
          && gfnt_lay_word_break_letter(cs->cps[u->cp_end - 1],
              options->line_break == GFNT_LINE_BREAK_STRICT
                  ? GUNI_LINE_BREAK_STRICT : GUNI_LINE_BREAK_NORMAL)
          && gfnt_lay_word_break_letter(cs->cps[u->cp_end],
              options->line_break == GFNT_LINE_BREAK_STRICT
                  ? GUNI_LINE_BREAK_STRICT : GUNI_LINE_BREAK_NORMAL)) {
        u->opportunity = true;
      }
      if (options->word_break == GFNT_WORD_BREAK_KEEP_ALL
          && options->line_break != GFNT_LINE_BREAK_ANYWHERE && u->cp_end < cs->n
          && gfnt_lay_keep_all_pair(cs->cps[u->cp_end - 1], cs->cps[u->cp_end],
              options->line_break == GFNT_LINE_BREAK_STRICT
                  ? GUNI_LINE_BREAK_STRICT : GUNI_LINE_BREAK_NORMAL)) {
        u->opportunity = false;
      }
      u->hard = gfnt_lay_paragraph_hard_break(p, last)
          && !(last == 0x0D && u->cp_end < cs->n && gfnt_lay_css_segment_break(cs->cps[u->cp_end]));
      if (u->object) {
        const GFNT_InlineObject * object = p->runs[u->run].object;
        u->opportunity = u->opportunity || object->break_after;
        if (u->cp_start && i > 0) {
          p->units[i - 1].opportunity = p->units[i - 1].opportunity || object->break_before;
        }
      }
    }
    for (size_t i = 1; i < p->unit_count; i++) {
      Unit * left = &p->units[i - 1];
      GFNT_F26Dot6 gap = gfnt_lay_autospace_boundary(p, left, &p->units[i]);

      left->autospace_after = gap;
    }
  }
  return r;
}

/** Hyphenation points, annotations, kashida, the tab width and the fallback metrics. */
static GFNT_Result create_finish(CreateState * cs) {
  const char * text = cs->text;
  const GFNT_TextSpan * spans = cs->spans;
  const GFNT_ParagraphOptions * options = cs->options;
  GFNT_Error * error = cs->error;
  GFNT_Paragraph * p = cs->p;
  GFNT_Result r = GFNT_OK;

  r = gfnt_lay_mark_hyphens(p, text, cs->span_cp, error);
  if (r != GFNT_OK) {
    return r;
  }
  r = gfnt_lay_create_annotations(p, options->annotations, options->annotation_count, error);
  if (r != GFNT_OK) {
    return r;
  }
  p->options.annotations = NULL;
  p->options.annotation_count = 0;
  r = gfnt_lay_find_kashida(p, error);
  if (r != GFNT_OK) {
    return r;
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

      r = gfnt_lay_shape_string(p, 0, &space, 1, g, &gn, &w, &tr, -1, error);
      if (r != GFNT_OK) {
        return r;
      }
      p->tab_width = w > 0 ? 8 * w : 2 * spans[0].style.size;
    }
  }
  p->fallback_metrics.face = spans[0].style.faces[0];
  gfnt_lay_scale_metrics(p->fallback_metrics.face, &spans[0].style, &p->fallback_metrics);
  if (!gfnt_lay_set_inline_metrics(&p->fallback_metrics, &spans[0].style)) {
    r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "the root span has an invalid line-height or vertical alignment");
    return r;
  }
  return r;
}

/** Release what the stages made and the paragraph did not take. */
static void create_state_free(CreateState * cs) {
  cs->allocator->free_fn(cs->allocator->ctx, cs->source_span_cp);
  cs->allocator->free_fn(cs->allocator->ctx, cs->source_to_layout);
  cs->allocator->free_fn(cs->allocator->ctx, cs->transformed_cps);
  cs->allocator->free_fn(cs->allocator->ctx, cs->transformed_bytes);
  cs->allocator->free_fn(cs->allocator->ctx, cs->transformed_spans);
  if (!cs->cps_owned) {
    cs->allocator->free_fn(cs->allocator->ctx, cs->cps);
  }
  cs->allocator->free_fn(cs->allocator->ctx, cs->levels);
  cs->allocator->free_fn(cs->allocator->ctx, cs->ori);
  cs->allocator->free_fn(cs->allocator->ctx, cs->collapsed);
  cs->allocator->free_fn(cs->allocator->ctx, cs->removed_break);
  cs->allocator->free_fn(cs->allocator->ctx, cs->span_cp);
  cs->allocator->free_fn(cs->allocator->ctx, cs->edge);
  cs->allocator->free_fn(cs->allocator->ctx, cs->opp);
  cs->allocator->free_fn(cs->allocator->ctx, cs->emergency_opp);
  cs->allocator->free_fn(cs->allocator->ctx, cs->breaks);
  cs->allocator->free_fn(cs->allocator->ctx, cs->scripts);
  gfnt_paragraph_free(cs->p);
}

GFNT_Result gfnt_paragraph_create(const char * text, size_t length,
    const GFNT_TextSpan * spans, size_t span_count,
    const GFNT_ParagraphOptions * options, const GFNT_Allocator * allocator,
    GFNT_Paragraph ** out, GFNT_Error * error) {
  static const GFNT_ParagraphOptions zero;
  CreateState state;
  CreateState * cs = &state;
  GFNT_Result r;

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
  r = create_validate(text, length, spans, span_count, options, error);
  if (r != GFNT_OK) {
    return r;
  }
  memset(&state, 0, sizeof state);
  state.text = text;
  state.length = length;
  state.spans = spans;
  state.span_count = span_count;
  state.options = options;
  state.allocator = allocator;
  state.error = error;
  state.emergency_breaks = options->overflow_wrap != GFNT_OVERFLOW_WRAP_NORMAL
      || options->word_break == GFNT_WORD_BREAK_BREAK_ALL
      || options->word_break == GFNT_WORD_BREAK_BREAK_WORD;
  r = create_allocate(cs);
  if (r != GFNT_OK) {
    goto done;
  }
  r = create_decode(cs);
  if (r != GFNT_OK) {
    goto done;
  }
  r = create_transform(cs);
  if (r != GFNT_OK) {
    goto done;
  }
  r = create_collapse(cs);
  if (r != GFNT_OK) {
    goto done;
  }
  r = create_styles(cs);
  if (r != GFNT_OK) {
    goto done;
  }
  r = create_analyse(cs);
  if (r != GFNT_OK) {
    goto done;
  }
  r = create_shape(cs);
  if (r != GFNT_OK) {
    goto done;
  }
  r = create_units(cs);
  if (r != GFNT_OK) {
    goto done;
  }
  r = create_finish(cs);
  if (r != GFNT_OK) {
    goto done;
  }
  *out = cs->p;
  cs->p = NULL;
done:
  create_state_free(cs);
  return r;
}
