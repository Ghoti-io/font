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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "kashida.h"
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
static GFNT_Result shape_string_dir(const GFNT_Paragraph * p, size_t span,
    const uint32_t * text, size_t text_count, PGlyph * glyphs, size_t * count,
    GFNT_F26Dot6 * total, PRun * run, int force, int rtl, size_t cap, GFNT_Error * error);
static uint8_t string_orientation(const GFNT_Paragraph * p, size_t span, uint32_t cp);
static int hyphen_orientation(const GFNT_Paragraph * p);

/** The width of a unit at @p pos from @p origin: a tab goes to the next stop. */
static int64_t eff_width_at(const GFNT_Paragraph * p, const Unit * u,
    int64_t pos, int64_t origin) {
  if (!u->tab || p->tab_width <= 0) {
    return u->width;
  }
  int64_t remainder = (pos + origin) % p->tab_width;
  if (remainder < 0) remainder += p->tab_width;
  return p->tab_width - remainder + u->ruby_added;
}

/** The width of a unit measured from the line start (the legacy tab origin). */
static int64_t eff_width(const GFNT_Paragraph * p, const Unit * u, int64_t pos) {
  return eff_width_at(p, u, pos, 0);
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
static bool set_inline_metrics(PRun * run, const GFNT_TextStyle * style) {
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
  allocator->free_fn(allocator->ctx, p->styles);
  allocator->free_fn(allocator->ctx, p->runs);
  allocator->free_fn(allocator->ctx, p->glyphs);
  allocator->free_fn(allocator->ctx, p->units);
  allocator->free_fn(allocator->ctx, p->annotations);
  allocator->free_fn(allocator->ctx, p->hyphen_width);
  allocator->free_fn(allocator->ctx, p->hyphen_cp);
  allocator->free_fn(allocator->ctx, p->kpoints);
  allocator->free_fn(allocator->ctx, p);
}

static bool annotation_style_equal(const GFNT_TextStyle * a, const GFNT_TextStyle * b) {
  return a->faces == b->faces && a->face_count == b->face_count && a->size == b->size
      && a->language == b->language && a->features == b->features
      && a->feature_count == b->feature_count && a->variation == b->variation
      && a->line_metrics == b->line_metrics && a->letter_spacing == b->letter_spacing
      && a->word_spacing == b->word_spacing
      && a->line_height.kind == b->line_height.kind
      && a->line_height.value == b->line_height.value
      && a->vertical_align == b->vertical_align
      && a->vertical_align_value == b->vertical_align_value
      && a->inline_object == b->inline_object;
}

static bool annotation_merge_compatible(const GFNT_Annotation * a, const GFNT_Annotation * b) {
  return a->merge && b->merge && a->kind == GFNT_ANNOTATION_RUBY
      && b->kind == GFNT_ANNOTATION_RUBY && a->base_end == b->base_start
      && a->position == b->position && a->align == b->align
      && a->group_break == b->group_break && a->overhang == b->overhang
      && !a->child_count && !b->child_count
      && annotation_style_equal(&a->style, &b->style);
}

static bool annotation_tree_valid(const GFNT_Annotation * items, size_t count,
    size_t text_length, size_t parent_start, size_t parent_end, size_t depth) {
  if (depth > 64 || (count && !items)) return false;
  for (size_t i = 0; i < count; i++) {
    const GFNT_Annotation * a = &items[i];
    const bool text_mark = a->kind == GFNT_ANNOTATION_RUBY
        || (a->kind == GFNT_ANNOTATION_EMPHASIS
            && a->mark_kind == GFNT_EMPHASIS_MARK_STRING);
    if (a->base_start >= a->base_end || a->base_end > text_length
        || a->base_start < parent_start || a->base_end > parent_end
        || (unsigned)a->kind > GFNT_ANNOTATION_EMPHASIS
        || (unsigned)a->position > GFNT_ANNOTATION_INTER_CHARACTER
        || (unsigned)a->align > GFNT_ANNOTATION_ALIGN_SPACE_AROUND
        || (unsigned)a->mark_kind > GFNT_EMPHASIS_MARK_STRING
        || (unsigned)a->overhang > GFNT_RUBY_OVERHANG_AUTO
        || !a->style.faces || !a->style.face_count || a->style.size <= 0
        || (a->merge && a->kind != GFNT_ANNOTATION_RUBY)
        || (a->kind == GFNT_ANNOTATION_RUBY && a->mark_kind != GFNT_EMPHASIS_MARK_DEFAULT)
        || (a->kind == GFNT_ANNOTATION_EMPHASIS && a->child_count)
        || (text_mark ? (!a->text || !a->text_length) : (a->text || a->text_length))
        || (a->child_count && !a->children)) return false;
    if (a->merge && !((i && annotation_merge_compatible(&items[i - 1], a))
            || (i + 1 < count && annotation_merge_compatible(a, &items[i + 1])))) return false;
    for (size_t j = 0; j < i; j++) {
      const GFNT_Annotation * b = &items[j];
      const bool disjoint = a->base_end <= b->base_start || b->base_end <= a->base_start;
      const bool coincident = a->base_start == b->base_start && a->base_end == b->base_end;
      if (!disjoint && !coincident) return false;
    }
    if (!annotation_tree_valid(a->children, a->child_count,
            a->text_length, 0, a->text_length, depth + 1)) return false;
  }
  return true;
}

static bool annotation_byte_cp(const GFNT_Paragraph * p, size_t byte, size_t * cp) {
  size_t lo = 0, hi = p->length + 1;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (p->cp_byte[mid] < byte) lo = mid + 1;
    else hi = mid;
  }
  if (lo > p->length || p->cp_byte[lo] != byte) return false;
  *cp = lo;
  return true;
}

static GFNT_Result shape_default_emphasis(const GFNT_Paragraph * p, const PAnnotation * source,
    uint32_t cp, PAnnotation * shaped, GFNT_Error * error);

static GFNT_Result create_annotations(GFNT_Paragraph * p, const GFNT_Annotation * items,
    size_t count, GFNT_Error * error) {
  const GFNT_Allocator * allocator = p->allocator;
  if (!count) return GFNT_OK;
  p->annotations = allocator->calloc_fn(allocator->ctx, count, sizeof *p->annotations);
  if (!p->annotations) return GFNT_ERR_OOM;
  p->annotation_count = count;
  size_t out_i = 0;
  for (size_t i = 0; i < count; i++) {
    GFNT_Annotation merged_source;
    const GFNT_Annotation * source = &items[i];
    size_t group_end = i + 1;
    size_t merged_text_length = source->text_length;
    while (group_end < count && annotation_merge_compatible(&items[group_end - 1],
            &items[group_end])) {
      merged_text_length += items[group_end].text_length;
      group_end++;
    }
    if (group_end > i + 1) {
      merged_source = *source;
      merged_source.base_end = items[group_end - 1].base_end;
      merged_source.text_length = merged_text_length;
      source = &merged_source;
    }
    PAnnotation * annotation = &p->annotations[out_i];
    GFNT_TextSpan span;
    GFNT_ParagraphOptions options = p->options;
    GFNT_Error child_error;
    GFNT_Layout annotation_layout = {0};
    size_t text_length = source->text_length;
    const char * text = source->text;
    GFNT_Result r;

    if (source->position == GFNT_ANNOTATION_INTER_CHARACTER && !p->vertical) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, source->base_start, GFNT_GLYPH_NONE,
          "inter-character ruby position requires vertical writing mode");
    }

    annotation->descriptor = *source;
    if (!annotation_byte_cp(p, source->base_start, &annotation->cp_start)
        || !annotation_byte_cp(p, source->base_end, &annotation->cp_end)) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, source->base_start, GFNT_GLYPH_NONE,
          "an annotation range ends inside a UTF-8 character");
    }
    if (source->kind == GFNT_ANNOTATION_EMPHASIS
        && source->mark_kind != GFNT_EMPHASIS_MARK_STRING) {
      static const char * const marks[] = {"•", "•", "◦", "◎", "▲", "﹅", ""};
      size_t base = annotation->cp_start;
      uint32_t cp = base < p->length ? p->cps[base] : 0;
      bool cjk = is_ideographic(cp);
      size_t mark = (size_t)source->mark_kind;
      bool filled = source->filled;
      if (source->mark_kind == GFNT_EMPHASIS_MARK_DEFAULT) {
        mark = cjk && p->vertical ? GFNT_EMPHASIS_MARK_SESAME : GFNT_EMPHASIS_MARK_DOT;
        filled = true;
      }
      text = !filled && mark == GFNT_EMPHASIS_MARK_DOT ? "◦"
          : !filled && mark == GFNT_EMPHASIS_MARK_TRIANGLE ? "△" : marks[mark];
      text_length = strlen(text);
    }
    annotation->unit_start = 0;
    while (annotation->unit_start < p->unit_count
        && p->units[annotation->unit_start].cp_end <= annotation->cp_start) {
      annotation->unit_start++;
    }
    annotation->unit_end = annotation->unit_start;
    while (annotation->unit_end < p->unit_count
        && p->units[annotation->unit_end].cp_start < annotation->cp_end) {
      const Unit * unit = &p->units[annotation->unit_end];
      if (unit->cp_start < annotation->cp_start || unit->cp_end > annotation->cp_end) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, source->base_start, GFNT_GLYPH_NONE,
            "an annotation range splits a shaped cluster");
      }
      annotation->unit_end++;
    }
    if (annotation->unit_start == annotation->unit_end) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, source->base_start, GFNT_GLYPH_NONE,
          "an annotation range contains no shaped text");
    }
    annotation->text = allocator->calloc_fn(allocator->ctx, text_length + 1, 1);
    if (!annotation->text) return GFNT_ERR_OOM;
    if (group_end > i + 1) {
      size_t at = 0;
      for (size_t j = i; j < group_end; j++) {
        memcpy(annotation->text + at, items[j].text, items[j].text_length);
        at += items[j].text_length;
      }
      text = annotation->text;
    }
    else if (text_length) memcpy(annotation->text, text, text_length);
    annotation->descriptor.text = annotation->text;
    annotation->descriptor.text_length = text_length;
    memset(&span, 0, sizeof span);
    span.length = text_length;
    span.style = source->style;
    options.direction = GFNT_BIDI_AUTO;
    options.align = GFNT_ALIGN_START;
    options.max_lines = 0;
    options.ellipsis = NULL;
    options.text_indent = 0;
    options.hanging_indent = false;
    options.annotations = source->children;
    options.annotation_count = source->child_count;
    r = gfnt_paragraph_create(annotation->text, text_length, &span, 1, &options,
        allocator, &annotation->shaped, &child_error);
    if (r != GFNT_OK) {
      return gfnt_error_set(error, r, child_error.table, source->base_start,
          child_error.glyph, "could not shape ruby annotation text");
    }
    if (gfnt_paragraph_max_content(annotation->shaped, &annotation->advance) != GFNT_OK) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, source->base_start, GFNT_GLYPH_NONE,
          "ruby annotation advance exceeds the supported range");
    }
    r = gfnt_paragraph_layout(annotation->shaped, GFNT_LAYOUT_UNBOUNDED,
        &annotation_layout, &child_error);
    if (r != GFNT_OK) {
      return gfnt_error_set(error, r, child_error.table, source->base_start,
          child_error.glyph, "could not measure ruby annotation text");
    }
    annotation->ascent = annotation_layout.lines[0].ascent;
    annotation->descent = annotation_layout.lines[0].descent;
    gfnt_layout_free(&annotation_layout);
    if (source->kind == GFNT_ANNOTATION_EMPHASIS) {
      if (source->mark_kind == GFNT_EMPHASIS_MARK_DEFAULT) {
        PAnnotation alternate = {0};
        const bool primary_cjk = strcmp(annotation->text, "﹅") == 0;
        const uint32_t alternate_cp = primary_cjk ? 'A' : 0x4E00;
        r = shape_default_emphasis(p, annotation, alternate_cp, &alternate, error);
        if (r != GFNT_OK) return r;
        annotation->alternate_shaped = alternate.shaped;
        annotation->alternate_advance = alternate.advance;
        annotation->alternate_ascent = alternate.ascent;
        annotation->alternate_descent = alternate.descent;
      }
      out_i++;
      i = group_end - 1;
      continue;
    }
    for (size_t u = annotation->unit_start; u < annotation->unit_end; u++) {
      if (p->units[u].hard) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, source->base_start, GFNT_GLYPH_NONE,
            "a ruby base range contains a hard line break");
      }
      if (source->group_break && u + 1 < annotation->unit_end) {
        p->units[u].opportunity = false;
        p->units[u].hyphen = false;
      }
    }
    out_i++;
    i = group_end - 1;
  }
  p->annotation_count = out_i;
  /* Reserve the full annotation advance for breaking. AUTO overhang is decided from the
   * actual rendered line below; it must never reduce this paragraph-wide reservation. */
  for (size_t i = 0; i < p->annotation_count; i++) {
    PAnnotation * a = &p->annotations[i];
    if (a->descriptor.kind != GFNT_ANNOTATION_RUBY) continue;
    bool first = true;
    GFNT_F26Dot6 base_width = 0, target = 0;
    for (size_t j = 0; j < i; j++) {
      if (p->annotations[j].descriptor.kind == GFNT_ANNOTATION_RUBY
          && p->annotations[j].cp_start == a->cp_start && p->annotations[j].cp_end == a->cp_end) {
        first = false;
        break;
      }
    }
    if (!first) continue;
    for (size_t u = a->unit_start; u < a->unit_end; u++) base_width += p->units[u].width;
    target = base_width;
    for (size_t j = i; j < p->annotation_count; j++) {
      PAnnotation * b = &p->annotations[j];
      if (b->descriptor.kind == GFNT_ANNOTATION_RUBY && b->cp_start == a->cp_start
          && b->cp_end == a->cp_end && b->advance > target) target = b->advance;
    }
    if (target > base_width) {
      GFNT_F26Dot6 auto_target = base_width;
      for (size_t j = i; j < p->annotation_count; j++) {
        PAnnotation * b = &p->annotations[j];
        if (b->descriptor.kind == GFNT_ANNOTATION_RUBY && b->cp_start == a->cp_start
            && b->cp_end == a->cp_end
            && b->descriptor.overhang == GFNT_RUBY_OVERHANG_AUTO
            && b->advance > auto_target) auto_target = b->advance;
      }
      if (auto_target == target) {
        for (size_t j = i; j < p->annotation_count; j++) {
          PAnnotation * b = &p->annotations[j];
          if (b->descriptor.kind == GFNT_ANNOTATION_RUBY && b->cp_start == a->cp_start
              && b->cp_end == a->cp_end) b->break_extra = target - base_width;
        }
      }
    }
    if (target > base_width && a->unit_end > a->unit_start) {
      const size_t n_units = a->unit_end - a->unit_start;
      const size_t divisor = a->descriptor.align == GFNT_ANNOTATION_ALIGN_SPACE_BETWEEN
          && n_units > 1 ? n_units - 1 : n_units;
      GFNT_F26Dot6 each = (target - base_width) / (GFNT_F26Dot6)divisor;
      GFNT_F26Dot6 rem = (target - base_width) % (GFNT_F26Dot6)divisor;
      for (size_t u = 0; u < n_units; u++) {
        GFNT_F26Dot6 share = each + (rem > 0 ? 1 : 0);
        if (rem > 0) rem--;
        if (a->descriptor.align == GFNT_ANNOTATION_ALIGN_START && u + 1 < n_units) share = 0;
        if (a->descriptor.align == GFNT_ANNOTATION_ALIGN_SPACE_BETWEEN && u + 1 == n_units) share = 0;
        Unit * unit = &p->units[a->unit_start + u];
        if (unit->g_hi > unit->g_lo) p->glyphs[unit->g_hi - 1].advance += share;
        unit->width += share;
        unit->ruby_added += share;
      }
    }
  }
  return GFNT_OK;
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


/** What drawing a stroke of some tatweels at a point makes of the glyphs around it. */
typedef struct {
  size_t g_lo, g_hi;           // the paragraph's glyphs the new ones replace
  size_t count;                // how many new ones
  size_t first;                // where they are in the layout's pool
  GFNT_F26Dot6 growth;         // their advance less that of the glyphs they replace
  GFNT_F26Dot6 adv1;           // the advance a tatweel adds: for guessing how many fit
} KReshape;

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
  r = shape_string_dir(p, pr->span, s, count, gw, &nw, &total, &run,
      GFNT_ORIENT_RUN_HORIZONTAL, rtl ? 1 : 0, KASHIDA_PROBE, error);
  if (r != GFNT_OK) {
    return r;
  }
  // The same without the tatweels.
  memmove(&s[at_tw], &s[at_tw + n_tw], (count - at_tw - n_tw) * sizeof s[0]);
  r = shape_string_dir(p, pr->span, s, count - n_tw, gc, &nc, &total, &run,
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
static GFNT_Result kashida_fit(const GFNT_Paragraph * p, const KPoint * pt, GFNT_F26Dot6 share,
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
static GFNT_Result find_kashida(GFNT_Paragraph * p, GFNT_Error * error) {
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

    if (hangs(p->cps[i]) || p->cps[i] == 0x09) {
      i++;
      continue;
    }
    while (end < p->length && !hangs(p->cps[end]) && p->cps[end] != 0x09) {
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
  bool * emergency_opp = NULL;
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
      || (unsigned)options->align_last > GFNT_ALIGN_LAST_JUSTIFY
      || (unsigned)options->overflow_wrap > GFNT_OVERFLOW_WRAP_BREAK_WORD
      || (unsigned)options->line_break > GFNT_LINE_BREAK_ANYWHERE
      || (unsigned)options->hyphens > GFNT_HYPHENS_MANUAL_PANGO
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
      if ((unsigned)s->line_height.kind > GFNT_LINE_HEIGHT_LENGTH
          || (s->line_height.kind == GFNT_LINE_HEIGHT_MULTIPLE && s->line_height.value < 0)
          || (s->line_height.kind == GFNT_LINE_HEIGHT_LENGTH && s->line_height.value < 0)
          || (unsigned)s->vertical_align > GFNT_VERTICAL_ALIGN_BOTTOM) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "a span has an invalid line-height or vertical alignment");
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
  if (!annotation_tree_valid(options->annotations, options->annotation_count,
          length, 0, length, 0)) {
    r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "annotation ranges or descriptors are invalid");
    goto done;
  }
  p->length = n;
  p->cps = cps;
  cps_owned = true;
  p->span_count = span_count;
  for (size_t s = 0; s < span_count; s++) {
    if (spans[s].style.inline_object) {
      if (spans[s].length != 3 || span_cp[s + 1] != span_cp[s] + 1
          || span_cp[s] >= n || cps[span_cp[s]] != 0xFFFC) {
        r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "an inline object descriptor must cover exactly one U+FFFC");
        goto done;
      }
    }
  }
  for (size_t i = 0; i < span_count; i++) {
    p->styles[i] = spans[i].style;
  }

  levels = ALLOC(levels, n);
  ori = ALLOC(ori, n);
  edge = ALLOC(edge, n + 1);
  opp = ALLOC(opp, n + 1);
  if (options->overflow_wrap != GFNT_OVERFLOW_WRAP_NORMAL) {
    emergency_opp = ALLOC(emergency_opp, n + 1);
  }
  if (!levels || !ori || !edge || !opp
      || (options->overflow_wrap != GFNT_OVERFLOW_WRAP_NORMAL && !emergency_opp)) {
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
    if (options->overflow_wrap != GFNT_OVERFLOW_WRAP_NORMAL) {
      bo.kind = GUNI_BREAK_GRAPHEME;
      (void)guni_break_all_codepoints(&bo, cps, n, NULL, 0, &break_count);
      allocator->free_fn(allocator->ctx, breaks);
      breaks = NULL;
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
            if (breaks[i] > 0 && breaks[i] < n) emergency_opp[breaks[i]] = true;
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
      if (end == start + 1 && cps[start] == 0xFFFC && style->inline_object) {
        const GFNT_InlineObject * object = style->inline_object;
        GFNT_TextStyle object_style = *style;
        PRun * run = &p->runs[p->run_count];
        PGlyph * glyph;
        Unit * unit;

        if (p->glyph_count == glyph_cap) {
          size_t cap = glyph_cap ? glyph_cap * 2 : 64;
          PGlyph * bigger;
          while (cap <= p->glyph_count) cap *= 2;
          bigger = allocator->realloc_fn(allocator->ctx, p->glyphs, cap * sizeof *bigger);
          if (!bigger) {
            r = gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
                "no memory for the inline object glyph");
            goto done;
          }
          p->glyphs = bigger;
          glyph_cap = cap;
        }
        memset(run, 0, sizeof *run);
        run->span = span;
        run->level = levels[start];
        run->orient = p->vertical ? GFNT_ORIENT_RUN_UPRIGHT : GFNT_ORIENT_RUN_HORIZONTAL;
        run->ascent = object->ascent;
        run->descent = object->descent;
        run->inline_ascent = object->ascent;
        run->inline_descent = object->descent;
        run->line_height = object->ascent + object->descent;
        run->object = object;
        object_style.vertical_align = object->align;
        object_style.vertical_align_value = object->align_value;
        if (!set_inline_metrics(run, &object_style)) {
          r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, p->cp_byte[start], GFNT_GLYPH_NONE,
              "an inline object has invalid computed line metrics");
          goto done;
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
          goto done;
        }
        unit->width = (GFNT_F26Dot6)((int64_t)object->advance + style->letter_spacing);
        unit->base_width = unit->width;
        p->glyphs[p->glyph_count].advance = unit->width;
        p->glyph_count++;
        p->run_count++;
        start = end;
        continue;
      }
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
        if (!set_inline_metrics(pr, style)) {
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
      u->emergency = emergency_opp && emergency_opp[u->cp_end];
      u->hard = is_newline(last)
          && !(last == 0x0D && u->cp_end < n && cps[u->cp_end] == 0x0A);
      if (u->object) {
        const GFNT_InlineObject * object = p->runs[u->run].object;
        u->opportunity = u->opportunity || object->break_after;
        if (u->cp_start && i > 0) {
          p->units[i - 1].opportunity = p->units[i - 1].opportunity || object->break_before;
        }
      }
    }
  }
  r = mark_hyphens(p, text, span_cp, error);
  if (r != GFNT_OK) {
    goto done;
  }
  r = create_annotations(p, options->annotations, options->annotation_count, error);
  if (r != GFNT_OK) {
    goto done;
  }
  p->options.annotations = NULL;
  p->options.annotation_count = 0;
  r = find_kashida(p, error);
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
  p->fallback_metrics.face = spans[0].style.faces[0];
  scale_metrics(p->fallback_metrics.face, &spans[0].style, &p->fallback_metrics);
  if (!set_inline_metrics(&p->fallback_metrics, &spans[0].style)) {
    r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "the root span has an invalid line-height or vertical alignment");
    goto done;
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
  allocator->free_fn(allocator->ctx, emergency_opp);
  allocator->free_fn(allocator->ctx, breaks);
  allocator->free_fn(allocator->ctx, scripts);
  gfnt_paragraph_free(p);
  return r;
}

/* ------------------------------------------------------------ content widths */

GFNT_Result gfnt_paragraph_min_content(const GFNT_Paragraph * p,
    GFNT_F26Dot6 * out) {
  int64_t total = 0, fit = 0, widest = 0;

  if (!p || !out) {
    return GFNT_ERR_INVALID;
  }
  for (size_t j = 0; j < p->unit_count; j++) {
    const Unit * u = &p->units[j];

    int64_t shown;

    total += eff_width(p, u, total);
    if (!u->space) {
      fit = total;
    }
    if (total > INT32_MAX || total < INT32_MIN || fit > INT32_MAX || fit < INT32_MIN) {
      return GFNT_ERR_INVALID;
    }
    // A stretch that ends at a hyphenation point ends in the hyphen; the end of the text is not one.
    shown = fit + (u->hyphen && u->opportunity && j + 1 < p->unit_count
                       ? p->hyphen_width[p->runs[u->run].span] : 0);
    if (shown > INT32_MAX || shown < INT32_MIN) return GFNT_ERR_INVALID;
    if (shown > widest) {
      widest = shown;
    }
    if (u->hard || u->opportunity
        || (p->options.overflow_wrap == GFNT_OVERFLOW_WRAP_ANYWHERE && u->emergency)) {
      total = fit = 0;
    }
  }
  {
    // A tab at the start of a stretch is a whole tab stop wide there and only the way to the next
    // stop in the unbroken line, so the stretches can add up to more than the line: a width at
    // which the line does not wrap needs no more than the line.
    GFNT_F26Dot6 max = 0;

    if (gfnt_paragraph_max_content(p, &max) != GFNT_OK) return GFNT_ERR_INVALID;
    if (widest > max) {
      widest = max;
    }
  }
  *out = widest;
  return GFNT_OK;
}

GFNT_Result gfnt_paragraph_max_content(const GFNT_Paragraph * p,
    GFNT_F26Dot6 * out) {
  int64_t total = 0, fit = 0, widest = 0;

  if (!p || !out) {
    return GFNT_ERR_INVALID;
  }
  for (size_t j = 0; j < p->unit_count; j++) {
    const Unit * u = &p->units[j];

    total += eff_width(p, u, total);
    if (!u->space) {
      fit = total;
    }
    if (total > INT32_MAX || total < INT32_MIN || fit > INT32_MAX || fit < INT32_MIN) {
      return GFNT_ERR_INVALID;
    }
    if (fit > widest) {
      widest = fit;
    }
    if (u->hard) {
      total = fit = 0;
    }
  }
  *out = (GFNT_F26Dot6)widest;
  return GFNT_OK;
}

/* ------------------------------------------------------------------ layout */

static void free_line_annotation_tree(const GFNT_Allocator * allocator,
    GFNT_LineAnnotation * annotations, size_t count) {
  if (!annotations) return;
  for (size_t ai = 0; ai < count; ai++) {
    GFNT_LineRun * runs = (GFNT_LineRun *)annotations[ai].runs;
    for (size_t ri = 0; ri < annotations[ai].run_count; ri++) {
      allocator->free_fn(allocator->ctx, (void *)runs[ri].glyphs);
    }
    allocator->free_fn(allocator->ctx, runs);
    free_line_annotation_tree(allocator,
        (GFNT_LineAnnotation *)annotations[ai].children, annotations[ai].child_count);
  }
  allocator->free_fn(allocator->ctx, annotations);
}

void gfnt_layout_free(GFNT_Layout * layout) {
  if (!layout || !layout->allocator) {
    return;
  }
  for (size_t li = 0; li < layout->line_count; li++) {
    GFNT_Line * line = &layout->lines[li];
    free_line_annotation_tree(layout->allocator,
        (GFNT_LineAnnotation *)line->annotations, line->annotation_count);
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
typedef struct {
  size_t end;
  bool hyphen;
} BreakResult;

/** Choose one line end without mutating paragraph state. */
/** The amount of a ruby group's reserved advance assigned to one base unit. */
static GFNT_F26Dot6 ruby_unit_share(const PAnnotation * annotation, size_t unit,
    GFNT_F26Dot6 extra) {
  const size_t n = annotation->unit_end - annotation->unit_start;
  if (!n || extra <= 0 || unit < annotation->unit_start || unit >= annotation->unit_end) return 0;
  const size_t relative = unit - annotation->unit_start;
  const size_t divisor = annotation->descriptor.align == GFNT_ANNOTATION_ALIGN_SPACE_BETWEEN
      && n > 1 ? n - 1 : n;
  /* Keep the divisor wide: an annotation can cover more units than fit in
   * GFNT_F26Dot6 even though its total reserved advance is still bounded. */
  uint64_t each = (uint64_t)extra / (uint64_t)divisor;
  uint64_t rem = (uint64_t)extra % (uint64_t)divisor;
  GFNT_F26Dot6 share = (GFNT_F26Dot6)(each + (relative < (size_t)rem ? 1 : 0));
  if (annotation->descriptor.align == GFNT_ANNOTATION_ALIGN_START && relative + 1 < n) share = 0;
  if (annotation->descriptor.align == GFNT_ANNOTATION_ALIGN_SPACE_BETWEEN
      && relative + 1 == n) share = 0;
  return share;
}

/** Return AUTO advance that a plain unit immediately after ruby can reclaim. */
static GFNT_F26Dot6 ruby_right_refund(const GFNT_Paragraph * p, size_t line_start,
    size_t neighbor, const PAnnotation ** group_out, size_t * group_start) {
  const Unit * unit;
  GFNT_F26Dot6 best = 0;
  size_t best_start = 0;
  const PAnnotation * best_group = NULL;
  if (neighbor >= p->unit_count) return 0;
  unit = &p->units[neighbor];
  if (unit->space || unit->tab || unit->hard || unit->object || unit->base_width <= 0) return 0;
  for (size_t ai = 0; ai < p->annotation_count; ai++) {
    const PAnnotation * a = &p->annotations[ai];
    bool first = true, neighbor_annotated = false, auto_sets_target = false;
    size_t first_unit;
    GFNT_F26Dot6 shares = 0;
    if (a->descriptor.kind != GFNT_ANNOTATION_RUBY || !a->break_extra
        || a->unit_end != neighbor) continue;
    for (size_t before = 0; before < ai; before++) {
      const PAnnotation * prior = &p->annotations[before];
      if (prior->descriptor.kind == GFNT_ANNOTATION_RUBY
          && prior->cp_start == a->cp_start && prior->cp_end == a->cp_end) {
        first = false;
        break;
      }
    }
    if (!first) continue;
    int64_t target = a->break_extra;
    for (size_t u = a->unit_start; u < a->unit_end; u++) {
      GFNT_F26Dot6 base_width = p->units[u].base_width;
      if (base_width > 0 && target > INT64_MAX - base_width) {
        target = INT64_MAX;
        break;
      }
      target += base_width;
    }
    for (size_t other = ai; other < p->annotation_count; other++) {
      const PAnnotation * candidate = &p->annotations[other];
      if (candidate->descriptor.kind == GFNT_ANNOTATION_RUBY
          && candidate->cp_start == a->cp_start && candidate->cp_end == a->cp_end
          && candidate->descriptor.overhang == GFNT_RUBY_OVERHANG_AUTO
          && candidate->advance >= target) {
        auto_sets_target = true;
        break;
      }
    }
    if (!auto_sets_target) continue;
    for (size_t other = 0; other < p->annotation_count; other++) {
      const PAnnotation * b = &p->annotations[other];
      if (b->unit_start <= neighbor && neighbor < b->unit_end) {
        neighbor_annotated = true;
        break;
      }
    }
    if (neighbor_annotated) continue;
    first_unit = a->unit_start > line_start ? a->unit_start : line_start;
    if (first_unit >= a->unit_end) continue;
    for (size_t u = first_unit; u < a->unit_end; u++) {
      shares += ruby_unit_share(a, u, a->break_extra);
    }
    if (shares > best) {
      best = shares;
      best_start = first_unit;
      best_group = a;
    }
  }
  if (best) {
    if (group_out) *group_out = best_group;
    if (group_start) *group_start = best_start;
  }
  return best;
}

static BreakResult break_one_line(const GFNT_Paragraph * p, size_t start,
    GFNT_F26Dot6 width, GFNT_F26Dot6 tab_origin, size_t * cand_at,
    GFNT_F26Dot6 * cand_fit) {
  BreakResult result = {p->unit_count, false};
  size_t i = start;
  GFNT_F26Dot6 total = 0, fit = 0;
  size_t emergency_at = 0;
  GFNT_F26Dot6 emergency_fit = 0;
  size_t cands = 0;
  bool have_emergency = false;
  bool bounded = width != GFNT_LAYOUT_UNBOUNDED;

  for (size_t j = i; j < p->unit_count; j++) {
    const Unit * u = &p->units[j];

    total += eff_width_at(p, u, total, tab_origin);
    total -= ruby_right_refund(p, start, j, NULL, NULL);
    if (!u->space) {
      fit = total;
    }
    if (u->hard) {
      result.end = j + 1;
      break;
    }
    if (bounded && u->emergency && j + 1 < p->unit_count && fit <= width) {
      emergency_at = j;
      emergency_fit = fit;
      have_emergency = true;
    }
    if (bounded && fit > width && cands) {
      size_t k = cands;

      while (k > 0) {
        const Unit * c = &p->units[cand_at[k - 1]];

        if (!c->hyphen
            || cand_fit[k - 1] + p->hyphen_width[p->runs[c->run].span] <= width) {
          break;
        }
        k--;
      }
      if (!k && have_emergency && emergency_at < j && emergency_fit <= width) {
        result.end = emergency_at + 1;
        break;
      }
      k = k ? k - 1 : 0;
      result.end = cand_at[k] + 1;
      result.hyphen = p->units[cand_at[k]].hyphen;
      break;
    }
    if (bounded && fit > width && !cands && have_emergency && emergency_at < j) {
      result.end = emergency_at + 1;
      break;
    }
    if (u->opportunity && j + 1 < p->unit_count) {
      if (bounded && fit > width) {
        result.end = j + 1;
        result.hyphen = u->hyphen;
        break;
      }
      cand_at[cands] = j;
      cand_fit[cands++] = fit;
    }
  }
  return result;
}

/** Apply the paragraph's first-line or hanging indent to a callback band. */
static bool indent_band(const GFNT_Paragraph * p, size_t line, GFNT_Band * band) {
  const bool apply = p->options.hanging_indent ? line > 0 : line == 0;
  if (!apply || !p->options.text_indent) return true;
  int64_t start = (int64_t)band->start + p->options.text_indent;
  int64_t width = (int64_t)band->width - p->options.text_indent;
  if (band->width == GFNT_LAYOUT_UNBOUNDED && p->options.text_indent < 0) {
    width = GFNT_LAYOUT_UNBOUNDED;
  }
  if (width < 0) width = 0;
  if (start < INT32_MIN || start > INT32_MAX || width > INT32_MAX
      || start + width > INT32_MAX) return false;
  band->start = (GFNT_F26Dot6)start;
  band->width = (GFNT_F26Dot6)width;
  return true;
}

static size_t break_lines(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    size_t * starts, bool * hyphened, size_t * cand_at, GFNT_F26Dot6 * cand_fit,
    GFNT_F26Dot6 * band_starts, GFNT_F26Dot6 * band_widths) {
  size_t lines = 0;
  size_t i = 0;

  if (p->unit_count == 0) {
    starts[0] = 0;
    hyphened[0] = false;
    GFNT_Band band = {0, width, 0};
    if (!indent_band(p, 0, &band)) return 1;
    band_starts[0] = band.start;
    band_widths[0] = band.width;
    return 1;
  }
  while (i < p->unit_count) {
    BreakResult result;

    starts[lines] = i;
    GFNT_Band band = {0, width, 0};
    if (!indent_band(p, lines, &band)) {
      // Creation has no error channel here; an unrepresentable indent is rejected by layout.
      band.width = width;
    }
    band_starts[lines] = band.start;
    band_widths[lines] = band.width;
    GFNT_F26Dot6 tab_origin = p->options.tabs_from_container ? band.start : 0;
    result = break_one_line(p, i, band.width, tab_origin, cand_at, cand_fit);
    hyphened[lines++] = result.hyphen;
    i = result.end;
  }
  // A text ending in a newline has an empty last line.
  if (p->units[p->unit_count - 1].hard) {
    GFNT_Band band = {0, width, 0};
    (void)indent_band(p, lines, &band);
    band_starts[lines] = band.start;
    band_widths[lines] = band.width;
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
  return shape_string_dir(p, span, text, text_count, glyphs, count, total, run, force, -1,
      ELL_GLYPHS, error);
}

/** As shape_string(), in a given direction: @p rtl 1 or 0, or -1 for the paragraph's own. */
static GFNT_Result shape_string_dir(const GFNT_Paragraph * p, size_t span,
    const uint32_t * text, size_t text_count,
    PGlyph * glyphs, size_t * count, GFNT_F26Dot6 * total, PRun * run, int force, int rtl,
    size_t cap, GFNT_Error * error) {
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
                         : (rtl >= 0 ? rtl : (p->para_level & 1)) ? GFNT_DIRECTION_RTL
                                                                  : GFNT_DIRECTION_LTR;
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
  if (!set_inline_metrics(run, style)) {
    gfnt_face_runs_free(&fr);
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "a span has invalid computed line metrics");
  }
  if (gfnt_face_units_per_em(run->face, &upem, NULL) != GFNT_OK || !upem) {
    upem = 1000;
  }
  int64_t scale_wide = ((int64_t)style->size * 1024 + upem / 2) / upem;
  if (scale_wide > INT32_MAX) {
    gfnt_face_runs_free(&fr);
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "span size exceeds the supported metric range");
  }
  scale = (GFNT_F16Dot16)scale_wide;
  for (size_t k = 0; k < fr.count; k++) {
    for (size_t g = 0; g < fr.runs[k].run.count && *count < cap; g++) {
      const GFNT_ShapedGlyph * sg = &fr.runs[k].run.glyphs[g];
      PGlyph * pg = &glyphs[(*count)++];

      memset(pg, 0, sizeof *pg);
      pg->glyph = sg->glyph;
      pg->cluster = (uint32_t)sg->cluster;
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
      else if (!u->object && !v->space && !v->tab && v->g_hi > v->g_lo
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


/** An ink box in line coordinates, or nothing. */
typedef struct {
  bool set;
  GFNT_F26Dot6 l, t, r, b;
} InkBox;

/** Marks a glyph a stroke's new glyphs replace, other than the first of them. */
#define KASHIDA_SKIP 0xFFFFFFFFu

/** Per-line measurements and inputs shared by placement and alignment. */
typedef struct {
  size_t unit_start, unit_end, hang_from;
  GFNT_F26Dot6 start_offset, available_width, top_y, thickness;
  GFNT_F26Dot6 natural, ascent, descent, gap;
  bool invalid;
} LayoutLineState;

typedef struct {
  const GFNT_Paragraph * paragraph;
  Slice * slices;
  size_t * order;
  GFNT_LineRun * runs;
  size_t * run_used;
  GFNT_PlacedGlyph * placed;
  size_t * glyph_used;
  uint8_t * gjf;
  uint32_t * gk;
  KReshape * kres;
  PGlyph * spool;
  GFNT_F26Dot6 * tabadv;
  GFNT_F26Dot6 * ruby_reclaim;
  GFNT_F26Dot6 extra, extra_rem, cursor, lead;
  size_t expanded;
  bool vertical, seen_text, ink_set;
  bool failed;
  GFNT_F26Dot6 line_ascent, line_descent;
  bool inline_mode;
  GFNT_F26Dot6 ink_l, ink_t, ink_r, ink_b;
} PlacementState;

typedef struct {
  const GFNT_Paragraph * paragraph;
  const GFNT_Allocator * allocator;
  uint8_t * gjf;
  uint32_t * gk;
  KReshape * kres;
  PGlyph ** spool;
  size_t * spool_len, * spool_cap;
} JustificationState;

static bool inline_shift(const GFNT_Paragraph * p, const PRun * run,
    GFNT_F26Dot6 line_ascent, GFNT_F26Dot6 line_descent, GFNT_F26Dot6 * out) {
  const PRun * root = &p->fallback_metrics;
  int64_t shift;
  switch (run->vertical_align) {
    case GFNT_VERTICAL_ALIGN_TOP: shift = (int64_t)line_ascent - run->inline_ascent; break;
    case GFNT_VERTICAL_ALIGN_BOTTOM: shift = (int64_t)run->inline_descent - line_descent; break;
    case GFNT_VERTICAL_ALIGN_TEXT_TOP: shift = (int64_t)root->inline_ascent - run->inline_ascent; break;
    case GFNT_VERTICAL_ALIGN_TEXT_BOTTOM: shift = (int64_t)run->inline_descent - root->inline_descent; break;
    case GFNT_VERTICAL_ALIGN_MIDDLE:
      shift = ((int64_t)root->x_height - run->inline_ascent + run->inline_descent) / 2;
      break;
    default: shift = run->shift; break;
  }
  if (shift < INT32_MIN || shift > INT32_MAX) return false;
  *out = (GFNT_F26Dot6)shift;
  return true;
}

/** Measure the shaped units that will be placed on one line. */
static LayoutLineState measure_line(const GFNT_Paragraph * p, size_t start, size_t end,
    GFNT_F26Dot6 width, GFNT_F26Dot6 top_y, GFNT_F26Dot6 tab_origin, GFNT_F26Dot6 * ew,
    GFNT_F26Dot6 * tabadv, GFNT_F26Dot6 * ruby_reclaim) {
  LayoutLineState state = {0};
  GFNT_F26Dot6 pos = 0;
  int64_t annotation_over = 0, annotation_under = 0;
  bool inline_mode = p->options.strut;
  bool inline_has_box = false, annotation_on_line = false;

  for (size_t j = start; j < end; j++) {
    const PRun * run = &p->runs[p->units[j].run];
    const GFNT_TextStyle * style = &p->styles[run->span];
    if (style->line_height.kind != GFNT_LINE_HEIGHT_NORMAL
        || run->vertical_align != GFNT_VERTICAL_ALIGN_BASELINE) inline_mode = true;
  }
  for (size_t i = 0; i < p->annotation_count; i++) {
    if (p->annotations[i].unit_end > start && p->annotations[i].unit_start < end) {
      inline_mode = true;
      annotation_on_line = true;
    }
  }
  if (inline_mode) {
    state.ascent = state.descent = INT32_MIN;
    state.gap = -1; /* sentinel: placement uses the union box */
  }

  state.unit_start = start;
  state.unit_end = end;
  state.available_width = width;
  state.top_y = top_y;
  for (size_t j = start; j < end; j++) {
    const Unit * unit = &p->units[j];
    const PRun * run = &p->runs[unit->run];
    size_t group_start = 0;
    const PAnnotation * group = NULL;
    GFNT_F26Dot6 refund = ruby_right_refund(p, start, j, &group, &group_start);

    if (unit->g_hi > unit->g_lo) ruby_reclaim[unit->g_hi - 1] = 0;
    if (refund) {
      if (!group) {
        state.invalid = true;
      }
      else {
        GFNT_F26Dot6 left = refund;
        for (size_t k = j; k > group_start && left > 0;) {
          Unit * base = &p->units[--k];
          GFNT_F26Dot6 share = ruby_unit_share(group, k, group->break_extra);
          if (share > left) share = left;
          if (share > ew[k]) {
            state.invalid = true;
            break;
          }
          ew[k] -= share;
          pos -= share;
          left -= share;
          if (base->g_hi > base->g_lo) ruby_reclaim[base->g_hi - 1] += share;
        }
        if (left) state.invalid = true;
      }
    }

    ew[j] = eff_width_at(p, unit, pos, tab_origin);
    pos += ew[j];
    if (unit->tab) {
      tabadv[unit->g_lo] = ew[j];
    }
    if (inline_mode) {
      int64_t a = run->inline_ascent, d = run->inline_descent;
      GFNT_F26Dot6 shift = 0;
      if (run->vertical_align == GFNT_VERTICAL_ALIGN_TOP
          || run->vertical_align == GFNT_VERTICAL_ALIGN_BOTTOM) continue;
      if (!inline_shift(p, run, 0, 0, &shift)) {
        state.invalid = true;
        continue;
      }
      a += shift; d -= shift;
      if (a < INT32_MIN || a > INT32_MAX || d < INT32_MIN || d > INT32_MAX) {
        state.invalid = true;
        continue;
      }
      inline_has_box = true;
      if (a > state.ascent) state.ascent = a;
      if (d > state.descent) state.descent = d;
    } else {
      int64_t thickness = (int64_t)run->ascent + run->descent + run->gap;
      if (thickness < INT32_MIN || thickness > INT32_MAX) {
        state.invalid = true;
        continue;
      }
      if (run->ascent > state.ascent) state.ascent = run->ascent;
      if (run->descent > state.descent) state.descent = run->descent;
      if (run->gap > state.gap) state.gap = run->gap;
      if (thickness > state.thickness) {
        state.thickness = (GFNT_F26Dot6)thickness;
      }
    }
  }
  if (inline_mode && p->options.strut) {
    const PRun * root = &p->fallback_metrics;
    int64_t a = (int64_t)root->inline_ascent + root->shift;
    int64_t d = (int64_t)root->inline_descent - root->shift;
    if (a < INT32_MIN || a > INT32_MAX || d < INT32_MIN || d > INT32_MAX) {
      state.invalid = true;
    }
    inline_has_box = true;
    if (a > state.ascent) state.ascent = a;
    if (d > state.descent) state.descent = d;
  }
  for (size_t i = 0; i < p->annotation_count; i++) {
    const PAnnotation * annotation = &p->annotations[i];
    int64_t height;
    if (annotation->unit_end <= start || annotation->unit_start >= end) continue;
    height = (int64_t)annotation->ascent + annotation->descent;
    if (annotation->descriptor.position == GFNT_ANNOTATION_UNDER) annotation_under += height;
    else annotation_over += height;
  }
  if (annotation_over > INT32_MAX || annotation_under > INT32_MAX) state.invalid = true;
  if (annotation_over > 0) {
    int64_t a = (int64_t)state.ascent + annotation_over;
    if (a > INT32_MAX) state.invalid = true;
    else state.ascent = (GFNT_F26Dot6)a;
  }
  if (annotation_under > 0) {
    int64_t d = (int64_t)state.descent + annotation_under;
    if (d > INT32_MAX) state.invalid = true;
    else state.descent = (GFNT_F26Dot6)d;
  }
  if (annotation_on_line) inline_has_box = true;
  if (inline_mode && !inline_has_box && start < end) {
    const PRun * run = &p->runs[p->units[start].run];
    state.ascent = run->inline_ascent;
    state.descent = run->inline_descent;
  }
  if (inline_mode && start < end) {
    for (size_t j = start; j < end; j++) {
      const PRun * run = &p->runs[p->units[j].run];
      int64_t a, d;
      GFNT_F26Dot6 shift;
      if (run->vertical_align != GFNT_VERTICAL_ALIGN_TOP
          && run->vertical_align != GFNT_VERTICAL_ALIGN_BOTTOM) continue;
      if (!inline_shift(p, run, state.ascent, state.descent, &shift)) {
        state.invalid = true;
        continue;
      }
      a = (int64_t)run->inline_ascent + shift;
      d = (int64_t)run->inline_descent - shift;
      if (a < INT32_MIN || a > INT32_MAX || d < INT32_MIN || d > INT32_MAX) {
        state.invalid = true;
        continue;
      }
      if (a > state.ascent) state.ascent = (GFNT_F26Dot6)a;
      if (d > state.descent) state.descent = (GFNT_F26Dot6)d;
    }
  }
  state.hang_from = end;
  while (state.hang_from > start && p->units[state.hang_from - 1].space) {
    state.hang_from--;
  }
  for (size_t j = start; j < state.hang_from; j++) {
    state.natural += ew[j];
  }
  if (start == end && !p->options.strut) {
    const PRun * run = start < p->unit_count ? &p->runs[p->units[start].run]
                                              : &p->fallback_metrics;
    const PRun * source = p->run_count ? (start > 0 ? &p->runs[p->units[start - 1].run] : run)
                                        : run;

    state.ascent = source->ascent;
    state.descent = source->descent;
    state.gap = source->gap;
    state.thickness = state.ascent + state.descent + state.gap;
  }
  if (inline_mode) {
    int64_t thickness = (int64_t)state.ascent + state.descent;
    if (thickness < INT32_MIN || thickness > INT32_MAX) state.invalid = true;
    else state.thickness = (GFNT_F26Dot6)thickness;
  }
  return state;
}

/** Distribute line slack through spaces and any eligible Arabic joining strokes. */
static GFNT_Result justify_line(JustificationState * state, GFNT_Line * line,
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
          r = kashida_fit(p, point, share, fit_glyphs, &fit, &fits, error);
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

/** Place the visual slices of a measured line, including glyph ink and justification. */
static void place_line(PlacementState * state, GFNT_Line * line, size_t slice_count) {
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
        GFNT_F26Dot6 shift = 0;
        if (slice->src == p->glyphs) advance -= state->ruby_reclaim[g];
        if (advance < 0) {
          state->failed = true;
          return;
        }
        if (!inline_shift(p, pr, state->line_ascent, state->line_descent, &shift)) {
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
        origin = state->cursor + before;
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
        && is_newline(p->cps[lo - 1]);
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
static GFNT_Rect align_lines(const GFNT_Paragraph * p, GFNT_Line * lines, size_t line_count,
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

typedef struct {
  const GFNT_Paragraph * paragraph;
  Slice * slices;
  uint8_t * levels;
  size_t * order;
  GFNT_Line * line;
  size_t unit_start, unit_end, hang_from, slice_count;
  bool hyphenated, truncated, vertical;
  PGlyph * hyphen_glyphs, * ellipsis_glyphs;
  size_t hyphen_count, ellipsis_count;
  PRun * hyphen_run, * ellipsis_run;
  LayoutLineState * measured;
  GFNT_F26Dot6 top_y;
} LineBuildState;

/** Storage and bounded scratch for one complete paragraph layout operation. */
typedef struct {
  const GFNT_Allocator * allocator;
  size_t * starts, * order, * cand_at;
  GFNT_F26Dot6 * band_starts, * band_widths, * line_tops;
  Slice * slices;
  uint8_t * slice_levels, * gjf;
  GFNT_F26Dot6 * ew, * tabadv, * cand_fit;
  GFNT_F26Dot6 * ruby_reclaim;
  uint32_t * gk;
  KReshape * kres;
  PGlyph * spool;
  size_t spool_len, spool_cap;
  bool * hyphened;
  GFNT_Line * lines;
  uint8_t * storage;
  GFNT_LineRun * runs;
  GFNT_PlacedGlyph * placed;
  size_t line_count, total_lines, runs_bytes, total_bytes, stroke_room;
  size_t run_used, glyph_used;
  bool more, any_truncated;
  GFNT_F26Dot6 y, widest;
  GFNT_Rect ink;
} LayoutState;

/** Reserve every paragraph and output buffer before placement begins. */
static GFNT_Result layout_state_allocate(LayoutState * state, const GFNT_Paragraph * p,
    GFNT_F26Dot6 width, const GFNT_LineConstraint * constraint, GFNT_Error * error) {
  const GFNT_Allocator * allocator = state->allocator;
  GFNT_F26Dot6 max_content;

  if (gfnt_paragraph_max_content(p, &max_content) != GFNT_OK) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "paragraph content width exceeds the 26.6 coordinate range");
  }

  state->starts = allocator->calloc_fn(allocator->ctx, p->unit_count + 2,
      sizeof *state->starts);
  state->slices = allocator->calloc_fn(allocator->ctx, p->unit_count + 1,
      sizeof *state->slices);
  state->order = allocator->calloc_fn(allocator->ctx, p->unit_count + 1,
      sizeof *state->order);
  state->slice_levels = allocator->calloc_fn(allocator->ctx, p->unit_count + 1, 1);
  state->ew = allocator->calloc_fn(allocator->ctx, p->unit_count + 1, sizeof *state->ew);
  state->tabadv = allocator->calloc_fn(allocator->ctx, p->glyph_count + 1,
      sizeof *state->tabadv);
  state->ruby_reclaim = allocator->calloc_fn(allocator->ctx, p->glyph_count + 1,
      sizeof *state->ruby_reclaim);
  state->gjf = allocator->calloc_fn(allocator->ctx, p->glyph_count + 1, 1);
  state->hyphened = allocator->calloc_fn(allocator->ctx, p->unit_count + 2,
      sizeof *state->hyphened);
  state->gk = allocator->calloc_fn(allocator->ctx, p->kcount ? p->glyph_count + 1 : 1,
      sizeof *state->gk);
  state->kres = allocator->calloc_fn(allocator->ctx, p->kcount + 1, sizeof *state->kres);
  state->cand_at = allocator->calloc_fn(allocator->ctx, p->unit_count + 1,
      sizeof *state->cand_at);
  state->cand_fit = allocator->calloc_fn(allocator->ctx, p->unit_count + 1,
      sizeof *state->cand_fit);
  state->band_starts = allocator->calloc_fn(allocator->ctx, p->unit_count + 2,
      sizeof *state->band_starts);
  state->band_widths = allocator->calloc_fn(allocator->ctx, p->unit_count + 2,
      sizeof *state->band_widths);
  state->line_tops = allocator->calloc_fn(allocator->ctx, p->unit_count + 2,
      sizeof *state->line_tops);
  if (!state->starts || !state->slices || !state->order || !state->slice_levels
      || !state->ew || !state->tabadv || !state->ruby_reclaim || !state->gjf
      || !state->hyphened || !state->gk
      || !state->kres || !state->cand_at || !state->cand_fit || !state->band_starts
      || !state->band_widths || !state->line_tops) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the layout");
  }
  if (!constraint) {
    state->line_count = break_lines(p, width, state->starts, state->hyphened,
        state->cand_at, state->cand_fit, state->band_starts, state->band_widths);
    GFNT_F26Dot6 top = 0;
    for (size_t li = 0; li < state->line_count; li++) {
      state->line_tops[li] = top;
      LayoutLineState measured = measure_line(p, state->starts[li], state->starts[li + 1],
          state->band_widths[li], top,
          p->options.tabs_from_container ? state->band_starts[li] : 0,
          state->ew, state->tabadv, state->ruby_reclaim);
      if (measured.invalid || measured.thickness < 0
          || top > INT32_MAX - measured.thickness) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "line metrics exceed the supported layout range");
      }
      top += measured.thickness;
    }
  }
  else {
    size_t unit = 0;
    GFNT_F26Dot6 top = 0;
    size_t attempts = 0;
    const size_t callback_budget = p->unit_count > (SIZE_MAX - 16) / 2
        ? SIZE_MAX : 2 * p->unit_count + 16;
    bool trailing_empty = false;
    do {
      GFNT_Band raw_band = {0}, band = {0};
      size_t line_start = unit;
      BreakResult br;
      LayoutLineState measured;
      GFNT_F26Dot6 height;
      size_t retries = 0;
      bool stable = false;
      bool have_candidate = false;
      GFNT_Result callback_result;

      do {
        if (!have_candidate) {
          if (++attempts > callback_budget) {
            return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
                "line constraint did not stabilize within its callback budget");
          }
          callback_result = constraint->band(constraint->ctx, top, 0, &raw_band);
          if (callback_result != GFNT_OK) return callback_result;
          if (raw_band.width < 0 || raw_band.next_top < 0
              || (int64_t)raw_band.start + raw_band.width > INT32_MAX) {
            return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
                "line constraint returned an invalid band");
          }
          band = raw_band;
          if (!indent_band(p, state->line_count, &band)) {
            return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
                "line indentation exceeds the supported coordinate range");
          }
          have_candidate = true;
        }
        if (!band.width && band.next_top > top) {
          top = band.next_top;
          have_candidate = false;
          continue;
        }
        GFNT_F26Dot6 tab_origin = p->options.tabs_from_container ? band.start : 0;
        br = break_one_line(p, unit, band.width, tab_origin, state->cand_at, state->cand_fit);
        measured = measure_line(p, unit, br.end, band.width, top, tab_origin, state->ew,
            state->tabadv, state->ruby_reclaim);
        if (measured.invalid || measured.thickness < 0) {
          return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
              "line metrics exceed the supported layout range");
        }
        height = measured.thickness;
        {
          GFNT_Band actual = {0};
          if (++attempts > callback_budget) {
            return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
                "line constraint did not stabilize within its callback budget");
          }
          callback_result = constraint->band(constraint->ctx, top, height, &actual);
          if (callback_result != GFNT_OK) return callback_result;
          if (actual.width < 0 || actual.next_top < 0
              || (int64_t)actual.start + actual.width > INT32_MAX) {
            return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
                "line constraint returned an invalid band");
          }
          if (actual.start == raw_band.start && actual.width == raw_band.width) {
            raw_band = actual;
            band = actual;
            if (!indent_band(p, state->line_count, &band)) {
              return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
                  "line indentation exceeds the supported coordinate range");
            }
            stable = true;
          }
          else {
            raw_band = actual;
            band = actual;
            if (!indent_band(p, state->line_count, &band)) {
              return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
                  "line indentation exceeds the supported coordinate range");
            }
            have_candidate = true;
            if (++retries > callback_budget) break;
          }
        }
      } while (!stable && retries <= callback_budget);
      if (!stable) {
        return gfnt_error_set(error, GFNT_ERR_LIMIT, 0, 0, GFNT_GLYPH_NONE,
            "line constraint changed continuously while measuring a line");
      }
      if (measured.natural > band.width && band.next_top > top) {
        top = band.next_top;
        have_candidate = false;
        continue;
      }
      state->starts[state->line_count] = line_start;
      state->hyphened[state->line_count] = br.hyphen;
      state->band_starts[state->line_count] = band.start;
      state->band_widths[state->line_count] = band.width;
      state->line_tops[state->line_count] = top;
      state->line_count++;
      if (br.end <= unit && unit < p->unit_count) {
        return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
            "line breaker did not advance");
      }
      unit = br.end;
      if (line_start == p->unit_count) {
        trailing_empty = false;
        break;
      }
      if (height > 0 && top > INT32_MAX - height) {
        return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
            "line block position exceeds the coordinate range");
      }
      top += height;
      if (unit == p->unit_count && p->unit_count
          && p->units[p->unit_count - 1].hard && !trailing_empty) {
        trailing_empty = true;
      }
      else if (unit == p->unit_count) {
        break;
      }
    } while (state->line_count < p->unit_count + 2);
    if (!state->line_count) state->line_count = 1;
  }
  state->starts[state->line_count] = p->unit_count;
  state->total_lines = state->line_count;
  if (p->options.max_lines && state->line_count > p->options.max_lines) {
    state->line_count = p->options.max_lines;
    state->more = true;
  }
  state->runs_bytes = (p->unit_count + state->line_count + 2) * sizeof *state->runs;
  state->stroke_room = p->kcount * (KASHIDA_COPIES_MAX + 4);
  if (p->kcount && width != GFNT_LAYOUT_UNBOUNDED) {
    const size_t per_line = (size_t)(width / p->kmin_adv) + 1;

    if (state->line_count <= (SIZE_MAX / 8) / per_line
        && state->line_count * per_line + 4 * p->kcount < state->stroke_room) {
      state->stroke_room = state->line_count * per_line + 4 * p->kcount;
    }
  }
  state->total_bytes = state->runs_bytes
      + (p->glyph_count + state->line_count * p->hyphen_glyphs + ELL_GLYPHS + 1
          + state->stroke_room) * sizeof *state->placed;
  state->lines = allocator->calloc_fn(allocator->ctx, state->line_count, sizeof *state->lines);
  state->storage = allocator->calloc_fn(allocator->ctx, state->total_bytes, 1);
  if (!state->lines || !state->storage) {
    return gfnt_error_set(error, GFNT_ERR_OOM, 0, 0, GFNT_GLYPH_NONE,
        "no memory for the layout");
  }
  state->runs = (GFNT_LineRun *)state->storage;
  state->placed = (GFNT_PlacedGlyph *)(state->storage + state->runs_bytes);
  return GFNT_OK;
}

static void layout_state_free(LayoutState * state) {
  const GFNT_Allocator * allocator = state->allocator;

  for (size_t li = 0; state->lines && li < state->line_count; li++) {
    GFNT_Line * line = &state->lines[li];
    free_line_annotation_tree(allocator,
        (GFNT_LineAnnotation *)line->annotations, line->annotation_count);
  }

  allocator->free_fn(allocator->ctx, state->starts);
  allocator->free_fn(allocator->ctx, state->band_starts);
  allocator->free_fn(allocator->ctx, state->band_widths);
  allocator->free_fn(allocator->ctx, state->line_tops);
  allocator->free_fn(allocator->ctx, state->slices);
  allocator->free_fn(allocator->ctx, state->order);
  allocator->free_fn(allocator->ctx, state->slice_levels);
  allocator->free_fn(allocator->ctx, state->ew);
  allocator->free_fn(allocator->ctx, state->tabadv);
  allocator->free_fn(allocator->ctx, state->ruby_reclaim);
  allocator->free_fn(allocator->ctx, state->gjf);
  allocator->free_fn(allocator->ctx, state->gk);
  allocator->free_fn(allocator->ctx, state->kres);
  allocator->free_fn(allocator->ctx, state->spool);
  allocator->free_fn(allocator->ctx, state->hyphened);
  allocator->free_fn(allocator->ctx, state->cand_at);
  allocator->free_fn(allocator->ctx, state->cand_fit);
  allocator->free_fn(allocator->ctx, state->lines);
  allocator->free_fn(allocator->ctx, state->storage);
}

static void free_annotation_runs(const GFNT_Allocator * allocator, GFNT_LineRun * runs,
    size_t count) {
  if (!runs) return;
  for (size_t i = 0; i < count; i++) {
    allocator->free_fn(allocator->ctx, (void *)runs[i].glyphs);
  }
  allocator->free_fn(allocator->ctx, runs);
}

static void free_annotation_tree(const GFNT_Allocator * allocator,
    GFNT_LineAnnotation * annotation) {
  free_annotation_runs(allocator, (GFNT_LineRun *)annotation->runs, annotation->run_count);
  free_line_annotation_tree(allocator, (GFNT_LineAnnotation *)annotation->children,
      annotation->child_count);
  annotation->runs = NULL;
  annotation->children = NULL;
}

static GFNT_Result clone_annotation_tree(const GFNT_Allocator * allocator,
    const GFNT_LineAnnotation * source, GFNT_F26Dot6 dx, GFNT_F26Dot6 dy,
    bool vertical, GFNT_LineAnnotation * out) {
  *out = *source;
  out->runs = NULL;
  out->children = NULL;
  out->box.x += dx;
  out->box.y += dy;
  GFNT_LineRun * runs = allocator->calloc_fn(allocator->ctx,
      source->run_count ? source->run_count : 1, sizeof *runs);
  if (!runs) return GFNT_ERR_OOM;
  out->runs = runs;
  for (size_t ri = 0; ri < source->run_count; ri++) {
    const GFNT_LineRun * in = &source->runs[ri];
    runs[ri] = *in;
    GFNT_PlacedGlyph * glyphs = allocator->calloc_fn(allocator->ctx,
        in->glyph_count ? in->glyph_count : 1, sizeof *glyphs);
    if (!glyphs) {
      out->run_count = ri;
      free_annotation_tree(allocator, out);
      return GFNT_ERR_OOM;
    }
    runs[ri].glyphs = glyphs;
    for (size_t gi = 0; gi < in->glyph_count; gi++) {
      glyphs[gi] = in->glyphs[gi];
      glyphs[gi].pen_x += dx;
      glyphs[gi].x += dx;
      glyphs[gi].pen_y += dy;
      glyphs[gi].y += dy;
    }
    runs[ri].pos += vertical ? dy : dx;
  }
  if (source->child_count) {
    GFNT_LineAnnotation * children = allocator->calloc_fn(allocator->ctx,
        source->child_count, sizeof *children);
    if (!children) {
      free_annotation_tree(allocator, out);
      return GFNT_ERR_OOM;
    }
    out->children = children;
    out->child_count = 0;
    for (size_t ci = 0; ci < source->child_count; ci++) {
      GFNT_Result r = clone_annotation_tree(allocator, &source->children[ci], dx, dy,
          vertical, &children[ci]);
      if (r != GFNT_OK) {
        free_annotation_tree(allocator, out);
        return r;
      }
      out->child_count++;
    }
  }
  return GFNT_OK;
}

static bool line_base_extent(const GFNT_Line * line, size_t base_start, size_t base_end,
    bool vertical, GFNT_F26Dot6 * lo, GFNT_F26Dot6 * hi) {
  bool found = false;
  for (size_t ri = 0; ri < line->run_count; ri++) {
    const GFNT_LineRun * run = &line->runs[ri];
    for (size_t gi = 0; gi < run->glyph_count; gi++) {
      const GFNT_PlacedGlyph * glyph = &run->glyphs[gi];
      if (glyph->cluster < base_start || glyph->cluster >= base_end) continue;
      GFNT_F26Dot6 start = vertical ? glyph->pen_y : glyph->pen_x;
      GFNT_F26Dot6 end = start + glyph->advance;
      if (!found || start < *lo) *lo = start;
      if (!found || end > *hi) *hi = end;
      found = true;
    }
  }
  return found;
}

static bool ruby_fragment_text_range(const PAnnotation * source, size_t cp_start,
    size_t cp_end, size_t * byte_start, size_t * byte_end) {
  size_t annotation_cps = 0, at = 0;
  while (at < source->descriptor.text_length) {
    uint32_t cp;
    size_t used = guni_utf8_decode(source->text + at, source->descriptor.text_length - at, &cp, NULL);
    if (!used) return false;
    at += used;
    annotation_cps++;
  }
  const size_t base_count = source->cp_end - source->cp_start;
  const size_t first = (cp_start - source->cp_start) * annotation_cps / base_count;
  const size_t last = (cp_end - source->cp_start) * annotation_cps / base_count;
  if (first == last) return false;
  *byte_start = *byte_end = 0;
  size_t ordinal = 0;
  at = 0;
  while (at < source->descriptor.text_length) {
    if (ordinal == first) *byte_start = at;
    if (ordinal == last) { *byte_end = at; break; }
    uint32_t cp;
    size_t used = guni_utf8_decode(source->text + at, source->descriptor.text_length - at, &cp, NULL);
    if (!used) return false;
    at += used;
    ordinal++;
  }
  if (last == annotation_cps) *byte_end = source->descriptor.text_length;
  return *byte_end > *byte_start;
}

static void free_annotation_slice(const GFNT_Allocator * allocator,
    GFNT_Annotation * annotations, size_t count) {
  if (!annotations) return;
  for (size_t i = 0; i < count; i++) {
    free_annotation_slice(allocator, (GFNT_Annotation *)annotations[i].children,
        annotations[i].child_count);
  }
  allocator->free_fn(allocator->ctx, annotations);
}

/** Copy nested annotations that overlap a byte slice of the paragraph that owns them. */
static GFNT_Result copy_annotation_slice(const GFNT_Paragraph * paragraph,
    size_t byte_start, size_t byte_end, GFNT_Annotation ** out, size_t * out_count) {
  const GFNT_Allocator * allocator = paragraph->allocator;
  size_t count = 0, written = 0;
  GFNT_Annotation * annotations;
  *out = NULL;
  *out_count = 0;
  for (size_t i = 0; i < paragraph->annotation_count; i++) {
    const GFNT_Annotation * descriptor = &paragraph->annotations[i].descriptor;
    if (descriptor->base_start < byte_end && descriptor->base_end > byte_start) count++;
  }
  if (!count) return GFNT_OK;
  annotations = allocator->calloc_fn(allocator->ctx, count, sizeof *annotations);
  if (!annotations) return GFNT_ERR_OOM;
  for (size_t i = 0; i < paragraph->annotation_count; i++) {
    const PAnnotation * source = &paragraph->annotations[i];
    const GFNT_Annotation * descriptor = &source->descriptor;
    size_t start = descriptor->base_start > byte_start ? descriptor->base_start : byte_start;
    size_t end = descriptor->base_end < byte_end ? descriptor->base_end : byte_end;
    size_t cp_start, cp_end, text_start = 0, text_end = source->descriptor.text_length;
    GFNT_Annotation * copy;
    if (start >= end) continue;
    if (!annotation_byte_cp(paragraph, start, &cp_start)
        || !annotation_byte_cp(paragraph, end, &cp_end)) {
      free_annotation_slice(allocator, annotations, written);
      return GFNT_ERR_INVALID;
    }
    copy = &annotations[written];
    *copy = *descriptor;
    copy->base_start = start - byte_start;
    copy->base_end = end - byte_start;
    copy->merge = false;
    copy->style = source->shaped->styles[0];
    copy->children = NULL;
    copy->child_count = 0;
    if (copy->kind == GFNT_ANNOTATION_RUBY) {
      const bool full_base = cp_start == source->cp_start && cp_end == source->cp_end;
      if (!full_base && !ruby_fragment_text_range(source, cp_start, cp_end,
              &text_start, &text_end)) continue;
      copy->text = source->text + text_start;
      copy->text_length = text_end - text_start;
      GFNT_Annotation * children = NULL;
      size_t child_count = 0;
      GFNT_Result r = copy_annotation_slice(source->shaped, text_start, text_end,
          &children, &child_count);
      if (r != GFNT_OK) {
        free_annotation_slice(allocator, annotations, written);
        free_annotation_slice(allocator, children, child_count);
        return r;
      }
      copy->children = children;
      copy->child_count = child_count;
    }
    else {
      copy->text = source->text;
      copy->text_length = source->descriptor.text_length;
    }
    written++;
  }
  *out = annotations;
  *out_count = written;
  return GFNT_OK;
}

static GFNT_Result ruby_fragment_cross_extent(const GFNT_Paragraph * parent,
    const PAnnotation * annotation, size_t cp_start, size_t cp_end,
    GFNT_F26Dot6 * extent, bool * present, GFNT_Error * error) {
  size_t byte_start, byte_end;
  GFNT_TextSpan span = {0};
  GFNT_ParagraphOptions options = parent->options;
  GFNT_Annotation * children = NULL;
  size_t child_count = 0;
  GFNT_Paragraph * fragment = NULL;
  GFNT_Layout layout = {0};
  GFNT_Error child_error;
  GFNT_Result r;
  *extent = 0;
  *present = false;
  if (!ruby_fragment_text_range(annotation, cp_start, cp_end, &byte_start, &byte_end))
    return GFNT_OK;
  *present = true;
  if (cp_start == annotation->cp_start && cp_end == annotation->cp_end) {
    *extent = annotation->ascent + annotation->descent;
    return GFNT_OK;
  }
  span.length = byte_end - byte_start;
  span.style = annotation->shaped->styles[0];
  options.direction = GFNT_BIDI_AUTO;
  options.align = GFNT_ALIGN_START;
  options.max_lines = 0;
  options.ellipsis = NULL;
  options.text_indent = 0;
  options.hanging_indent = false;
  r = copy_annotation_slice(annotation->shaped, byte_start, byte_end, &children, &child_count);
  if (r != GFNT_OK) return r;
  options.annotations = children;
  options.annotation_count = child_count;
  r = gfnt_paragraph_create(annotation->text + byte_start, span.length, &span, 1,
      &options, parent->allocator, &fragment, &child_error);
  free_annotation_slice(parent->allocator, children, child_count);
  if (r != GFNT_OK) return gfnt_error_set(error, r, child_error.table, cp_start,
      child_error.glyph, "could not shape a same-side ruby fragment");
  r = gfnt_paragraph_layout(fragment, GFNT_LAYOUT_UNBOUNDED, &layout, error);
  if (r == GFNT_OK && layout.line_count)
    *extent = layout.lines[0].ascent + layout.lines[0].descent;
  gfnt_layout_free(&layout);
  gfnt_paragraph_free(fragment);
  return r;
}

static GFNT_Result append_line_annotation(const GFNT_Paragraph * p, const PAnnotation * source,
    size_t index, GFNT_Line * base_line, size_t base_start, size_t base_end,
    size_t base_cp_start, size_t base_cp_end,
    GFNT_LineAnnotation * out, GFNT_Error * error) {
  GFNT_Layout shaped = {0};
  GFNT_Paragraph * fragment_paragraph = NULL;
  PAnnotation fragment_source;
  const PAnnotation * placement = source;
  const GFNT_Allocator * allocator = p->allocator;
  const GFNT_Line * source_line;
  GFNT_F26Dot6 lo = 0, hi = 0, target, base_lo, shift_along, shift_across, dx, dy;
  int64_t same_side_total = 0, same_side_before = 0;
  GFNT_F26Dot6 base_cross_half = 0;
  bool vertical = p->vertical;
  bool rtl = source->unit_start < p->unit_count
      && (p->runs[p->units[source->unit_start].run].level & 1);
  GFNT_Result r;

  if (source->descriptor.kind == GFNT_ANNOTATION_RUBY
      && (base_cp_start != source->cp_start || base_cp_end != source->cp_end)) {
    size_t byte_first, byte_last;
    if (!ruby_fragment_text_range(source, base_cp_start, base_cp_end,
            &byte_first, &byte_last)) return GFNT_OK;
    GFNT_TextSpan span = {0};
    GFNT_ParagraphOptions options = p->options;
    GFNT_Error child_error;
    GFNT_Annotation * children = NULL;
    size_t child_count = 0;
    span.length = byte_last - byte_first;
    span.style = source->shaped->styles[0];
    options.direction = GFNT_BIDI_AUTO;
    options.align = GFNT_ALIGN_START;
    options.max_lines = 0;
    options.ellipsis = NULL;
    options.text_indent = 0;
    options.hanging_indent = false;
    r = copy_annotation_slice(source->shaped, byte_first, byte_last,
        &children, &child_count);
    if (r != GFNT_OK) return gfnt_error_set(error, r, 0, base_start,
        GFNT_GLYPH_NONE, "could not copy nested ruby annotations for a line fragment");
    options.annotations = children;
    options.annotation_count = child_count;
    r = gfnt_paragraph_create(source->text + byte_first, span.length, &span, 1, &options,
        allocator, &fragment_paragraph, &child_error);
    free_annotation_slice(allocator, children, child_count);
    if (r != GFNT_OK) return gfnt_error_set(error, r, child_error.table, base_start,
        child_error.glyph, "could not shape a ruby line fragment");
    fragment_source = *source;
    fragment_source.shaped = fragment_paragraph;
    if (gfnt_paragraph_max_content(fragment_paragraph, &fragment_source.advance) != GFNT_OK) {
      gfnt_paragraph_free(fragment_paragraph);
      return GFNT_ERR_INVALID;
    }
    placement = &fragment_source;
  }
  r = gfnt_paragraph_layout(placement->shaped, GFNT_LAYOUT_UNBOUNDED, &shaped, error);
  if (r != GFNT_OK) { gfnt_paragraph_free(fragment_paragraph); return r; }
  if (!shaped.line_count) {
    gfnt_layout_free(&shaped);
    gfnt_paragraph_free(fragment_paragraph);
    return GFNT_OK;
  }
  source_line = &shaped.lines[0];
  if (vertical) {
    for (size_t ui = 0; ui < p->unit_count; ui++) {
      const Unit * unit = &p->units[ui];
      if (unit->cp_start >= base_cp_end || unit->cp_end <= base_cp_start) continue;
      const PRun * run = &p->runs[unit->run];
      const GFNT_F26Dot6 thickness = run->ascent + run->descent;
      if (thickness / 2 > base_cross_half) base_cross_half = thickness / 2;
    }
  }
  if (source->descriptor.kind == GFNT_ANNOTATION_RUBY
      && source->descriptor.position != GFNT_ANNOTATION_INTER_CHARACTER) {
    for (size_t ai = 0; ai < p->annotation_count; ai++) {
      const PAnnotation * other = &p->annotations[ai];
      if (other->descriptor.kind != GFNT_ANNOTATION_RUBY
          || other->descriptor.position == GFNT_ANNOTATION_INTER_CHARACTER
          || other->cp_start != source->cp_start || other->cp_end != source->cp_end) continue;
      GFNT_F26Dot6 fragment_extent = 0;
      bool fragment_present = false;
      r = ruby_fragment_cross_extent(p, other, base_cp_start, base_cp_end,
          &fragment_extent, &fragment_present, error);
      if (r != GFNT_OK) {
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return r;
      }
      if (!fragment_present) continue;
      const int64_t extent = fragment_extent;
      if (other->descriptor.position != source->descriptor.position) continue;
      if (extent < 0 || same_side_total > INT32_MAX - extent) {
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      same_side_total += extent;
      if (ai < index) same_side_before += extent;
    }
  }
  if (!line_base_extent(base_line, base_start, base_end, vertical, &lo, &hi)) {
    lo = vertical ? base_line->offset : base_line->offset;
    hi = lo;
  }
  target = hi - lo;
  base_lo = lo;
  if (placement->descriptor.align == GFNT_ANNOTATION_ALIGN_START && rtl) {
    lo = hi - source_line->extent;
  }
  else if (placement->descriptor.align == GFNT_ANNOTATION_ALIGN_CENTER
      || placement->descriptor.align == GFNT_ANNOTATION_ALIGN_SPACE_AROUND
      || placement->descriptor.align == GFNT_ANNOTATION_ALIGN_SPACE_BETWEEN) {
    lo += (target - source_line->extent) / 2;
  }
  if (source->descriptor.overhang == GFNT_RUBY_OVERHANG_AUTO) {
    const GFNT_F26Dot6 line_start = vertical ? base_line->box.y : base_line->box.x;
    const GFNT_F26Dot6 line_end = line_start + (vertical ? base_line->box.height : base_line->box.width);
    GFNT_F26Dot6 left_room = 0, right_room = 0;
    bool left_plain = false, right_plain = false;
    if (source->unit_start > 0) {
      const Unit * u = &p->units[source->unit_start - 1];
      size_t byte = p->cp_byte[u->cp_start];
      left_plain = !u->space && !u->tab && !u->hard && byte >= base_line->start && byte < base_line->end;
      for (size_t ai = 0; ai < p->annotation_count && left_plain; ai++) {
        const PAnnotation * a = &p->annotations[ai];
        if (a->cp_start < u->cp_end && a->cp_end > u->cp_start) left_plain = false;
      }
      if (left_plain) left_room = u->width;
    }
    if (source->unit_end < p->unit_count) {
      const Unit * u = &p->units[source->unit_end];
      size_t byte = p->cp_byte[u->cp_start];
      right_plain = !u->space && !u->tab && !u->hard && byte >= base_line->start && byte < base_line->end;
      for (size_t ai = 0; ai < p->annotation_count && right_plain; ai++) {
        const PAnnotation * a = &p->annotations[ai];
        if (a->cp_start < u->cp_end && a->cp_end > u->cp_start) right_plain = false;
      }
      if (right_plain) right_room = u->width;
    }
    if (!left_plain) left_room = 0;
    if (!right_plain) right_room = 0;
    /* Center alignment has already split excess equally. Restrict that centered placement to
     * the neighbor space actually available; do not shift it a second time by the overhang. */
    const GFNT_F26Dot6 min_lo = base_lo - left_room;
    const GFNT_F26Dot6 max_lo = base_lo + target + right_room - source_line->extent;
    if (lo < min_lo) lo = min_lo;
    if (lo > max_lo) lo = max_lo;
    bool anchored_to_neighbor = false;
    if (source_line->extent >= line_end - line_start && source->unit_end < p->unit_count) {
      const Unit * neighbor = &p->units[source->unit_end];
      const size_t neighbor_start = p->cp_byte[neighbor->cp_start];
      const size_t neighbor_end = p->cp_byte[neighbor->cp_end];
      GFNT_F26Dot6 neighbor_lo = 0, neighbor_hi = 0;
      if (right_plain && !neighbor->space && !neighbor->tab && !neighbor->hard && !neighbor->object
          && neighbor_start >= base_line->start && neighbor_end <= base_line->end
          && line_base_extent(base_line, neighbor_start, neighbor_end, vertical,
              &neighbor_lo, &neighbor_hi)) {
        if (neighbor_hi <= base_lo) {
          /* In RTL text the logical successor sits before the base in visual order. Let
           * an over-wide ruby stay attached to that visual edge, even if it overflows. */
          lo = neighbor_hi;
          anchored_to_neighbor = true;
        }
        else if (neighbor_lo >= hi) {
          /* In LTR text the logical successor sits after the base. */
          lo = neighbor_lo - source_line->extent;
          anchored_to_neighbor = true;
        }
      }
    }
    if (!anchored_to_neighbor) {
      if (lo < line_start) lo = line_start;
      if (lo + source_line->extent > line_end) lo = line_end - source_line->extent;
      if (lo < line_start) lo = line_start;
    }
  }
  if (vertical) {
    const int64_t along = (int64_t)lo - source_line->offset;
    if (along < INT32_MIN || along > INT32_MAX) {
      gfnt_layout_free(&shaped);
      gfnt_paragraph_free(fragment_paragraph);
      return GFNT_ERR_LIMIT;
    }
    shift_along = (GFNT_F26Dot6)along;
    const bool sideways_lr = p->mode == GFNT_WRITING_SIDEWAYS_LR;
    const bool place_left = sideways_lr
        ? source->descriptor.position == GFNT_ANNOTATION_OVER
        : source->descriptor.position == GFNT_ANNOTATION_UNDER;
    if (source->descriptor.position == GFNT_ANNOTATION_INTER_CHARACTER) {
      const int64_t value = (int64_t)base_line->baseline + base_line->ascent;
      if (value < INT32_MIN || value > INT32_MAX) {
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
    else if (source->descriptor.kind != GFNT_ANNOTATION_RUBY) {
      const int64_t value = source->descriptor.position == GFNT_ANNOTATION_UNDER
          ? (int64_t)base_line->baseline - base_line->descent + source_line->ascent
          : (int64_t)base_line->baseline + base_line->ascent - source->ascent;
      if (value < INT32_MIN || value > INT32_MAX) {
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
    else if (place_left) {
      const int64_t extent = (int64_t)source_line->ascent + source_line->descent;
      const int64_t value = (int64_t)base_line->baseline - base_cross_half
          - same_side_before - extent;
      if (value < INT32_MIN || value > INT32_MAX) {
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
    else {
      const int64_t value = (int64_t)base_line->baseline + base_cross_half
          + same_side_before;
      if (value < INT32_MIN || value > INT32_MAX) {
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
  }
  else {
    const int64_t along = (int64_t)lo - source_line->offset;
    if (along < INT32_MIN || along > INT32_MAX) {
      gfnt_layout_free(&shaped);
      gfnt_paragraph_free(fragment_paragraph);
      return GFNT_ERR_LIMIT;
    }
    shift_along = (GFNT_F26Dot6)along;
    if (source->descriptor.kind != GFNT_ANNOTATION_RUBY) {
      const int64_t value = source->descriptor.position == GFNT_ANNOTATION_UNDER
          ? (int64_t)base_line->baseline + base_line->descent - source_line->descent
          : (int64_t)base_line->baseline - base_line->ascent + source_line->ascent;
      if (value < INT32_MIN || value > INT32_MAX) {
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
    else if (source->descriptor.position == GFNT_ANNOTATION_UNDER) {
      const int64_t value = (int64_t)base_line->baseline + base_line->descent
          - same_side_total + same_side_before + source_line->ascent;
      if (value < INT32_MIN || value > INT32_MAX) {
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
    else {
      const int64_t extent = (int64_t)source_line->ascent + source_line->descent;
      const int64_t value = (int64_t)base_line->baseline - base_line->ascent
          + same_side_total - same_side_before - extent + source_line->ascent;
      if (value < INT32_MIN || value > INT32_MAX) {
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
  }
  {
    const int64_t cross_extent = (int64_t)source_line->ascent + source_line->descent;
    const int64_t box_x = vertical ? shift_across : lo;
    const int64_t box_y = vertical ? lo : (int64_t)shift_across - source_line->ascent;
    const int64_t box_width = vertical ? cross_extent : source_line->extent;
    const int64_t box_height = vertical ? source_line->extent : cross_extent;
    const int64_t dx64 = vertical ? (int64_t)shift_across - source_line->baseline : shift_along;
    const int64_t dy64 = vertical ? shift_along : (int64_t)shift_across - source_line->baseline;
    if (cross_extent < 0 || box_x < INT32_MIN || box_x > INT32_MAX
        || box_y < INT32_MIN || box_y > INT32_MAX || box_width < 0 || box_width > INT32_MAX
        || box_height < 0 || box_height > INT32_MAX || dx64 < INT32_MIN || dx64 > INT32_MAX
        || dy64 < INT32_MIN || dy64 > INT32_MAX
        || box_x + box_width > INT32_MAX || box_y + box_height > INT32_MAX) {
      gfnt_layout_free(&shaped);
      gfnt_paragraph_free(fragment_paragraph);
      return GFNT_ERR_LIMIT;
    }
    out->box = (GFNT_Rect){(GFNT_F26Dot6)box_x, (GFNT_F26Dot6)box_y,
        (GFNT_F26Dot6)box_width, (GFNT_F26Dot6)box_height};
    dx = (GFNT_F26Dot6)dx64;
    dy = (GFNT_F26Dot6)dy64;
  }
  if (source_line->ink.width > 0 && source_line->ink.height > 0) {
    GFNT_Rect ink = source_line->ink;
    const int64_t ink_x = (int64_t)ink.x + dx;
    const int64_t ink_y = (int64_t)ink.y + dy;
    const int64_t ink_right = ink_x + ink.width;
    const int64_t ink_bottom = ink_y + ink.height;
    if (ink_x < INT32_MIN || ink_x > INT32_MAX || ink_y < INT32_MIN || ink_y > INT32_MAX
        || ink_right > INT32_MAX || ink_bottom > INT32_MAX) {
      gfnt_layout_free(&shaped);
      gfnt_paragraph_free(fragment_paragraph);
      return GFNT_ERR_LIMIT;
    }
    ink.x = (GFNT_F26Dot6)ink_x;
    ink.y = (GFNT_F26Dot6)ink_y;
    if (base_line->ink.width <= 0 || base_line->ink.height <= 0) base_line->ink = ink;
    else {
      const int64_t old_right = (int64_t)base_line->ink.x + base_line->ink.width;
      const int64_t old_bottom = (int64_t)base_line->ink.y + base_line->ink.height;
      const int64_t left = ink.x < base_line->ink.x ? ink.x : base_line->ink.x;
      const int64_t top = ink.y < base_line->ink.y ? ink.y : base_line->ink.y;
      const int64_t right = old_right > ink_right ? old_right : ink_right;
      const int64_t bottom = old_bottom > ink_bottom ? old_bottom : ink_bottom;
      if (old_right > INT32_MAX || old_bottom > INT32_MAX || right > INT32_MAX
          || bottom > INT32_MAX || right - left > INT32_MAX || bottom - top > INT32_MAX) {
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      base_line->ink.x = (GFNT_F26Dot6)left;
      base_line->ink.y = (GFNT_F26Dot6)top;
      base_line->ink.width = (GFNT_F26Dot6)(right - left);
      base_line->ink.height = (GFNT_F26Dot6)(bottom - top);
    }
  }
  out->base_start = base_start;
  out->base_end = base_end;
  out->kind = source->descriptor.kind;
  out->position = source->descriptor.position;
  out->annotation_index = index;
  out->run_count = source_line->run_count;
  GFNT_LineRun * runs = allocator->calloc_fn(allocator->ctx,
      out->run_count ? out->run_count : 1, sizeof *runs);
  if (!runs) {
    gfnt_layout_free(&shaped);
    gfnt_paragraph_free(fragment_paragraph);
    return GFNT_ERR_OOM;
  }
  out->runs = runs;
  for (size_t ri = 0; ri < out->run_count; ri++) {
    const GFNT_LineRun * in = &source_line->runs[ri];
    runs[ri] = *in;
    GFNT_PlacedGlyph * glyphs = allocator->calloc_fn(allocator->ctx,
        in->glyph_count ? in->glyph_count : 1, sizeof *glyphs);
    if (!glyphs) {
      free_annotation_runs(allocator, runs, ri);
      out->runs = NULL;
      out->run_count = 0;
      gfnt_layout_free(&shaped);
      gfnt_paragraph_free(fragment_paragraph);
      return GFNT_ERR_OOM;
    }
    runs[ri].glyphs = glyphs;
    for (size_t gi = 0; gi < in->glyph_count; gi++) {
      glyphs[gi] = in->glyphs[gi];
      const int64_t pen_x = vertical ? shift_across : (int64_t)glyphs[gi].pen_x + shift_along;
      const int64_t pen_y = vertical ? (int64_t)glyphs[gi].pen_y + shift_along : shift_across;
      const int64_t glyph_x = vertical
          ? (int64_t)glyphs[gi].x + dx : (int64_t)glyphs[gi].x + shift_along;
      const int64_t glyph_y = vertical
          ? (int64_t)glyphs[gi].y + shift_along : (int64_t)glyphs[gi].y + dy;
      if (pen_x < INT32_MIN || pen_x > INT32_MAX || pen_y < INT32_MIN || pen_y > INT32_MAX
          || glyph_x < INT32_MIN || glyph_x > INT32_MAX
          || glyph_y < INT32_MIN || glyph_y > INT32_MAX) {
        free_annotation_runs(allocator, runs, ri + 1);
        out->runs = NULL;
        out->run_count = 0;
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      glyphs[gi].pen_x = (GFNT_F26Dot6)pen_x;
      glyphs[gi].pen_y = (GFNT_F26Dot6)pen_y;
      glyphs[gi].x = (GFNT_F26Dot6)glyph_x;
      glyphs[gi].y = (GFNT_F26Dot6)glyph_y;
    }
    {
      const int64_t pos = (int64_t)runs[ri].pos + shift_along;
      if (pos < INT32_MIN || pos > INT32_MAX) {
        free_annotation_runs(allocator, runs, ri + 1);
        out->runs = NULL;
        out->run_count = 0;
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return GFNT_ERR_LIMIT;
      }
      runs[ri].pos = (GFNT_F26Dot6)pos;
    }
  }
  out->child_count = source_line->annotation_count;
  if (out->child_count) {
    GFNT_LineAnnotation * children = allocator->calloc_fn(allocator->ctx,
        out->child_count, sizeof *children);
    if (!children) {
      free_annotation_runs(allocator, runs, out->run_count);
      out->runs = NULL;
      out->run_count = 0;
      gfnt_layout_free(&shaped);
      gfnt_paragraph_free(fragment_paragraph);
      return GFNT_ERR_OOM;
    }
    out->children = children;
    out->child_count = 0;
    for (size_t ci = 0; ci < source_line->annotation_count; ci++) {
      GFNT_Result child_result = clone_annotation_tree(allocator,
          &source_line->annotations[ci], dx, dy, vertical, &children[ci]);
      if (child_result != GFNT_OK) {
        free_annotation_runs(allocator, runs, out->run_count);
        out->runs = NULL;
        out->run_count = 0;
        free_line_annotation_tree(allocator, children, out->child_count);
        out->children = NULL;
        out->child_count = 0;
        gfnt_layout_free(&shaped);
        gfnt_paragraph_free(fragment_paragraph);
        return child_result;
      }
      out->child_count++;
    }
  }
  gfnt_layout_free(&shaped);
  gfnt_paragraph_free(fragment_paragraph);
  return GFNT_OK;
}

static GFNT_Result shape_default_emphasis(const GFNT_Paragraph * p, const PAnnotation * source,
    uint32_t cp, PAnnotation * shaped, GFNT_Error * error) {
  static const char dot[] = "•";
  static const char sesame[] = "﹅";
  const bool cjk_vertical = p->vertical && is_ideographic(cp);
  const char * text = cjk_vertical ? sesame : dot;
  const size_t length = cjk_vertical ? sizeof sesame - 1 : sizeof dot - 1;
  GFNT_TextSpan span = {0};
  GFNT_ParagraphOptions options = p->options;
  GFNT_Layout layout = {0};
  GFNT_Result r;

  span.length = length;
  span.style = source->descriptor.style;
  options.direction = GFNT_BIDI_AUTO;
  options.align = GFNT_ALIGN_START;
  options.max_lines = 0;
  options.ellipsis = NULL;
  options.text_indent = 0;
  options.hanging_indent = false;
  options.annotations = NULL;
  options.annotation_count = 0;
  r = gfnt_paragraph_create(text, length, &span, 1, &options, p->allocator,
      &shaped->shaped, error);
  if (r != GFNT_OK) return r;
  r = gfnt_paragraph_max_content(shaped->shaped, &shaped->advance);
  if (r == GFNT_OK) r = gfnt_paragraph_layout(shaped->shaped, GFNT_LAYOUT_UNBOUNDED,
      &layout, error);
  if (r == GFNT_OK && layout.line_count) {
    shaped->ascent = layout.lines[0].ascent;
    shaped->descent = layout.lines[0].descent;
  }
  gfnt_layout_free(&layout);
  if (r != GFNT_OK) gfnt_paragraph_free(shaped->shaped);
  return r;
}

static GFNT_Result add_line_annotations(const GFNT_Paragraph * p, GFNT_Line * lines,
    size_t line_count, GFNT_Error * error) {
  const GFNT_Allocator * allocator = p->allocator;
  for (size_t li = 0; li < line_count; li++) {
    GFNT_Line * line = &lines[li];
    size_t count = 0;
    for (size_t ai = 0; ai < p->annotation_count; ai++) {
      const PAnnotation * annotation = &p->annotations[ai];
      size_t first = p->cp_byte[annotation->cp_start];
      size_t last = p->cp_byte[annotation->cp_end];
      if (annotation->descriptor.kind == GFNT_ANNOTATION_RUBY) {
        if (line->end > first && line->start < last) {
          size_t cp_start = annotation->cp_start, cp_end = annotation->cp_end;
          while (cp_start < cp_end && p->cp_byte[cp_start + 1] <= line->start) cp_start++;
          while (cp_end > cp_start && p->cp_byte[cp_end - 1] >= line->end) cp_end--;
          size_t fragment_start, fragment_end;
          if ((annotation->descriptor.group_break || (cp_start == annotation->cp_start
                      && cp_end == annotation->cp_end))
              || ruby_fragment_text_range(annotation, cp_start, cp_end,
                  &fragment_start, &fragment_end)) count++;
        }
      }
      else {
        for (size_t u = annotation->unit_start; u < annotation->unit_end; u++) {
          const Unit * unit = &p->units[u];
          if (unit->cp_end <= annotation->cp_start || unit->cp_start >= annotation->cp_end
              || unit->space || unit->tab || is_newline(p->cps[unit->cp_start])) continue;
          if (p->cp_byte[unit->cp_start] >= line->start && p->cp_byte[unit->cp_start] < line->end) {
            count++;
          }
        }
      }
    }
    if (!count) continue;
    GFNT_LineAnnotation * records = allocator->calloc_fn(allocator->ctx, count, sizeof *records);
    if (!records) return GFNT_ERR_OOM;
    line->annotations = records;
    for (size_t ai = 0; ai < p->annotation_count; ai++) {
      const PAnnotation * annotation = &p->annotations[ai];
      const size_t first = p->cp_byte[annotation->cp_start];
      const size_t last = p->cp_byte[annotation->cp_end];
      if (annotation->descriptor.kind == GFNT_ANNOTATION_RUBY) {
        if (line->end > first && line->start < last) {
          size_t cp_start = annotation->cp_start, cp_end = annotation->cp_end;
          while (cp_start < cp_end && p->cp_byte[cp_start + 1] <= line->start) cp_start++;
          while (cp_end > cp_start && p->cp_byte[cp_end - 1] >= line->end) cp_end--;
          size_t fragment_start, fragment_end;
          const bool full_group = annotation->descriptor.group_break
              || (cp_start == annotation->cp_start && cp_end == annotation->cp_end);
          if (!full_group && !ruby_fragment_text_range(annotation, cp_start, cp_end,
                  &fragment_start, &fragment_end)) continue;
          size_t base_start = full_group ? first : p->cp_byte[cp_start];
          size_t base_end = full_group ? last : p->cp_byte[cp_end];
          GFNT_Result r = append_line_annotation(p, annotation, ai, line, base_start, base_end,
              full_group ? annotation->cp_start : cp_start,
              full_group ? annotation->cp_end : cp_end,
              &records[line->annotation_count], error);
          if (r != GFNT_OK) return r;
          line->annotation_count++;
        }
      }
      else {
        for (size_t u = annotation->unit_start; u < annotation->unit_end; u++) {
          const Unit * unit = &p->units[u];
          size_t start = p->cp_byte[unit->cp_start], end = p->cp_byte[unit->cp_end];
          if (unit->space || unit->tab || is_newline(p->cps[unit->cp_start])
              || start < line->start || start >= line->end) continue;
          const bool use_default = annotation->descriptor.mark_kind == GFNT_EMPHASIS_MARK_DEFAULT;
          const bool cjk_vertical = p->vertical && is_ideographic(p->cps[unit->cp_start]);
          PAnnotation alternate_mark;
          const PAnnotation * emitted = annotation;
          GFNT_Result r;
          if (use_default && cjk_vertical != (strcmp(annotation->text, "﹅") == 0)) {
            alternate_mark = *annotation;
            alternate_mark.shaped = annotation->alternate_shaped;
            alternate_mark.advance = annotation->alternate_advance;
            alternate_mark.ascent = annotation->alternate_ascent;
            alternate_mark.descent = annotation->alternate_descent;
            emitted = &alternate_mark;
          }
          r = append_line_annotation(p, emitted, ai, line, start, end,
              unit->cp_start, unit->cp_end, &records[line->annotation_count], error);
          if (r != GFNT_OK) return r;
          line->annotation_count++;
        }
      }
    }
  }
  return GFNT_OK;
}

/** Transfer the completed buffers to the public result, then release scratch storage. */
static void layout_state_finish(LayoutState * state, GFNT_Layout * out,
    const GFNT_Paragraph * p, GFNT_F26Dot6 width, bool vertical, GFNT_F26Dot6 y,
    GFNT_F26Dot6 widest, size_t line_count, bool truncated, GFNT_Rect ink) {
  out->lines = state->lines;
  out->line_count = line_count;
  out->writing_mode = p->mode;
  out->width = vertical ? y : width == GFNT_LAYOUT_UNBOUNDED ? widest : width;
  out->height = vertical ? (width == GFNT_LAYOUT_UNBOUNDED ? widest : width) : y;
  out->truncated = truncated;
  out->ink = ink;
  out->allocator = state->allocator;
  out->storage_ = state->storage;
  state->lines = NULL;
  state->storage = NULL;
  layout_state_free(state);
}

/** Assemble logical units into bidi-reordered visual run slices and establish the line box. */
static GFNT_Result build_line_slices(LineBuildState * state, GFNT_Error * error) {
  const GFNT_Paragraph * p = state->paragraph;
  const size_t ua = state->unit_start, ub = state->unit_end;
  GFNT_Line * line = state->line;
  size_t ns = 0;

  for (size_t j = ua; j < ub; j++) {
    const Unit * unit = &p->units[j];
    const PRun * run = &p->runs[unit->run];
    bool hang = j >= state->hang_from;
    uint8_t level = hang ? p->para_level : run->level;
    Slice * slice = ns ? &state->slices[ns - 1] : NULL;

    if (!slice || slice->pr != run || slice->level != level || slice->hang != hang) {
      slice = &state->slices[ns++];
      slice->pr = run;
      slice->src = p->glyphs;
      slice->ellipsis = false;
      slice->hyphen = false;
      slice->level = level;
      slice->hang = hang;
      slice->g_lo = unit->g_lo;
      slice->g_hi = unit->g_hi;
      slice->b_lo = p->cp_byte[unit->cp_start];
      slice->b_hi = p->cp_byte[unit->cp_end];
      slice->advance = 0;
    }
    if (p->cp_byte[unit->cp_start] < slice->b_lo) slice->b_lo = p->cp_byte[unit->cp_start];
    if (p->cp_byte[unit->cp_end] > slice->b_hi) slice->b_hi = p->cp_byte[unit->cp_end];
    if (unit->g_lo < slice->g_lo) slice->g_lo = unit->g_lo;
    if (unit->g_hi > slice->g_hi) slice->g_hi = unit->g_hi;
  }
  if (state->hyphenated) {
    const size_t at = p->cp_byte[p->units[ub - 1].cp_end];
    Slice * slice = &state->slices[ns++];

    for (size_t g = 0; g < state->hyphen_count; g++) {
      state->hyphen_glyphs[g].cluster = (uint32_t)at;
    }
    slice->pr = state->hyphen_run;
    slice->src = state->hyphen_glyphs;
    slice->ellipsis = false;
    slice->hyphen = true;
    slice->level = state->hyphen_run->level;
    slice->hang = false;
    slice->g_lo = 0;
    slice->g_hi = state->hyphen_count;
    slice->b_lo = slice->b_hi = at;
    slice->advance = 0;
  }
  if (state->truncated) {
    size_t at = ub > ua ? p->cp_byte[p->units[ub - 1].cp_end]
        : (ua < p->unit_count ? p->cp_byte[p->units[ua].cp_start] : p->cp_byte[p->length]);
    Slice * slice = &state->slices[ns++];

    for (size_t g = 0; g < state->ellipsis_count; g++) {
      state->ellipsis_glyphs[g].cluster = (uint32_t)at;
    }
    slice->pr = state->ellipsis_run;
    slice->src = state->ellipsis_glyphs;
    slice->ellipsis = true;
    slice->hyphen = false;
    slice->level = p->para_level;
    slice->hang = false;
    slice->g_lo = 0;
    slice->g_hi = state->ellipsis_count;
    slice->b_lo = slice->b_hi = at;
    slice->advance = 0;
  }
  for (size_t k = 0; k < ns; k++) state->levels[k] = state->slices[k].level;
  if (ns && guni_bidi_reorder(state->levels, ns, state->order, p->unit_count + 1) != GUNI_OK) {
    return gfnt_error_set(error, GFNT_ERR_INTERNAL, 0, 0, GFNT_GLYPH_NONE,
        "the line could not be put in visual order");
  }
  line->start = ua < p->unit_count ? p->cp_byte[p->units[ua].cp_start] : p->cp_byte[p->length];
  line->end = ub > ua ? p->cp_byte[p->units[ub - 1].cp_end] : line->start;
  if (state->vertical) {
    line->box.x = state->measured->top_y;
    line->box.width = state->measured->thickness;
    line->ascent = line->descent = state->measured->thickness / 2;
  }
  else {
    line->box.y = state->measured->top_y;
    line->box.height = state->measured->gap < 0
        ? state->measured->ascent + state->measured->descent
        : state->measured->ascent + state->measured->descent + state->measured->gap;
    line->ascent = state->measured->ascent;
    line->descent = state->measured->descent;
    line->baseline = state->top_y + (state->measured->gap < 0 ? 0 : state->measured->gap / 2)
        + state->measured->ascent;
  }
  state->slice_count = ns;
  return GFNT_OK;
}

/** Record the phase results and apply physical alignment to every line. */
static void layout_process_finish(LayoutState * state, const GFNT_Paragraph * p,
    GFNT_F26Dot6 width, bool vertical, bool para_rtl, PGlyph * spool,
    size_t spool_len, size_t spool_cap, size_t run_used, size_t glyph_used,
    bool truncated, GFNT_F26Dot6 y, GFNT_F26Dot6 widest) {
  state->spool = spool;
  state->spool_len = spool_len;
  state->spool_cap = spool_cap;
  state->run_used = run_used;
  state->glyph_used = glyph_used;
  state->any_truncated = truncated;
  state->y = y;
  state->widest = widest;
  state->ink = align_lines(p, state->lines, state->line_count,
      state->band_starts, state->band_widths, width, widest, y, para_rtl, vertical);
}

/** Run measurement, justification, visual-run construction, placement, then alignment. */
static GFNT_Result layout_process_lines(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    bool vertical, bool para_rtl, LayoutState * state, GFNT_Error * error) {
  const GFNT_Allocator * allocator = state->allocator;
  size_t * starts = state->starts, * order = state->order;
  Slice * slices = state->slices;
  uint8_t * slice_levels = state->slice_levels, * gjf = state->gjf;
  GFNT_F26Dot6 * ew = state->ew, * tabadv = state->tabadv;
  uint32_t * gk = state->gk;
  KReshape * kres = state->kres;
  PGlyph * spool = state->spool;
  size_t spool_len = state->spool_len, spool_cap = state->spool_cap;
  bool * hyphened = state->hyphened;
  GFNT_Line * lines = state->lines;
  GFNT_LineRun * runs = state->runs;
  GFNT_PlacedGlyph * placed = state->placed;
  size_t line_count = state->line_count, total_lines = state->total_lines;
  GFNT_F26Dot6 * band_starts = state->band_starts, * band_widths = state->band_widths;
  GFNT_F26Dot6 * line_tops = state->line_tops;
  size_t run_used = state->run_used, glyph_used = state->glyph_used;
  bool more = state->more, any_truncated = false;
  GFNT_F26Dot6 y = 0, widest = 0;
  GFNT_Result r = GFNT_OK;

  for (size_t li = 0; li < line_count; li++) {
    size_t ua = starts[li];
    size_t ub = starts[li + 1];
    GFNT_Line * line = &lines[li];
    GFNT_F26Dot6 line_width = band_widths[li];
    GFNT_F26Dot6 line_top = line_tops[li];
    GFNT_F26Dot6 tab_origin = p->options.tabs_from_container ? band_starts[li] : 0;
    LayoutLineState line_state = measure_line(p, ua, ub, line_width, line_top, tab_origin,
        ew, tabadv, state->ruby_reclaim);
    size_t hang_from;
    size_t ns = 0;
    PlacementState placement = {0};
    PGlyph ell_g[ELL_GLYPHS];
    size_t ell_n = 0;
    GFNT_F26Dot6 ell_w = 0, natural = 0, extra = 0, extra_rem = 0;
    PRun ell_run;
    PGlyph hy_g[ELL_GLYPHS];
    size_t hy_n = 0;
    GFNT_F26Dot6 hy_w = 0;
    PRun hy_run;
    bool hy_line = false;
    bool trunc_line = false;
    bool bounded = line_width != GFNT_LAYOUT_UNBOUNDED;

    line_state.start_offset = band_starts[li];

    if (line_state.invalid) {
      r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "line metrics exceed the supported layout range");
      break;
    }

    hang_from = line_state.hang_from;
    natural = line_state.natural;
    if (hyphened[li] && ub > ua) {
      // The line ends inside a word: a hyphen follows its last letter, and takes room.
      const PRun * last = &p->runs[p->units[ub - 1].run];

      r = shape_string(p, last->span, &p->hyphen_cp[last->span], 1, hy_g, &hy_n, &hy_w,
          &hy_run, hyphen_orientation(p), error);
      if (r != GFNT_OK) {
        state->spool = spool;
        state->spool_len = spool_len;
        state->spool_cap = spool_cap;
        return r;
      }
      hy_run.level = last->level;
      hy_line = true;
      natural += hy_w;
    }
    if (p->options.max_lines && p->ell_count && li + 1 == line_count
        && (more || (bounded && natural > line_width))) {
      size_t span = ub > ua ? p->runs[p->units[ub - 1].run].span
                            : (ua < p->unit_count ? p->runs[p->units[ua].run].span
                                                  : 0);

      r = shape_string(p, span, p->ell, p->ell_count, ell_g, &ell_n, &ell_w,
          &ell_run, -1, error);
      if (r != GFNT_OK) {
        state->spool = spool;
        state->spool_len = spool_len;
        state->spool_cap = spool_cap;
        return r;
      }
      if (bounded) {
        GFNT_F26Dot6 room = line_width - ell_w, used = 0;
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
    {
      JustificationState justification = {
          p, allocator, gjf, gk, kres, &spool, &spool_len, &spool_cap
      };

      r = justify_line(&justification, line, ua, ub, hang_from, natural,
          line_width, li, total_lines, bounded, trunc_line,
          &extra, &extra_rem, error);
      if (r != GFNT_OK) {
        state->spool = spool;
        state->spool_len = spool_len;
        state->spool_cap = spool_cap;
        return r;
      }
    }
    {
      LineBuildState build = {
        p, slices, slice_levels, order, line, ua, ub, hang_from, 0,
        hy_line, trunc_line, vertical, hy_g, ell_g, hy_n, ell_n,
        &hy_run, &ell_run, &line_state, line_top
      };

      r = build_line_slices(&build, error);
      if (r != GFNT_OK) {
        state->spool = spool;
        state->spool_len = spool_len;
        state->spool_cap = spool_cap;
        return r;
      }
      ns = build.slice_count;
    }
    placement.paragraph = p;
    placement.slices = slices;
    placement.order = order;
    placement.runs = runs;
    placement.run_used = &run_used;
    placement.placed = placed;
    placement.glyph_used = &glyph_used;
    placement.gjf = gjf;
    placement.gk = gk;
    placement.kres = kres;
    placement.spool = spool;
    placement.tabadv = tabadv;
    placement.ruby_reclaim = state->ruby_reclaim;
    placement.extra = extra;
    placement.extra_rem = extra_rem;
    placement.vertical = vertical;
    placement.inline_mode = line_state.gap < 0;
    placement.line_ascent = line_state.ascent;
    placement.line_descent = line_state.descent;
    place_line(&placement, line, ns);
    if (placement.failed) {
      r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
          "line alignment exceeds the supported layout range");
      break;
    }
    line->truncated = trunc_line;
    line->offset = line_state.start_offset - placement.lead;
    if (line->extent > widest) {
      widest = line->extent;
    }
    {
      GFNT_F26Dot6 line_height = vertical ? line->box.width : line->box.height;
      if (line_top <= INT32_MAX - line_height && line_top + line_height > y) {
        y = line_top + line_height;
      }
    }
  }
  layout_process_finish(state, p, width, vertical, para_rtl, spool, spool_len,
      spool_cap, run_used, glyph_used, any_truncated, y, widest);
  return r;
}

static bool layout_union_rect(GFNT_Rect * bounds, const GFNT_Rect * rect);

static GFNT_Result paragraph_layout_internal(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    const GFNT_LineConstraint * constraint, GFNT_Layout * out, GFNT_Error * error);

GFNT_Result gfnt_paragraph_layout(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    GFNT_Layout * out, GFNT_Error * error) {
  return paragraph_layout_internal(p, width, NULL, out, error);
}

static GFNT_Result paragraph_layout_internal(const GFNT_Paragraph * p, GFNT_F26Dot6 width,
    const GFNT_LineConstraint * constraint, GFNT_Layout * out, GFNT_Error * error) {
  LayoutState state = {0};
  bool para_rtl;
  const bool vertical = p ? p->vertical : false;
  GFNT_Result r = GFNT_OK;

  gfnt_error_clear(error);
  if (!p || !out || width < 0 || (constraint && !constraint->band)) return GFNT_ERR_INVALID;
  if (!constraint && width != GFNT_LAYOUT_UNBOUNDED
      && (int64_t)width - p->options.text_indent > INT32_MAX) {
    return gfnt_error_set(error, GFNT_ERR_INVALID, 0, 0, GFNT_GLYPH_NONE,
        "line indentation exceeds the supported coordinate range");
  }
  para_rtl = (p->para_level & 1) != 0;
  state.allocator = p->allocator;
  r = layout_state_allocate(&state, p, constraint ? GFNT_LAYOUT_UNBOUNDED : width,
      constraint, error);
  if (r != GFNT_OK) goto done;
  r = layout_process_lines(p, constraint ? 0 : width, vertical, para_rtl, &state, error);
  if (r != GFNT_OK) goto done;
  r = add_line_annotations(p, state.lines, state.line_count, error);
  if (r != GFNT_OK) goto done;
  state.ink = (GFNT_Rect){0};
  bool ink_set = false;
  for (size_t li = 0; li < state.line_count; li++) {
    const GFNT_Rect * ink = &state.lines[li].ink;
    if (ink->width <= 0 || ink->height <= 0) continue;
    if (!ink_set) {
      state.ink = *ink;
      ink_set = true;
    }
    else if (!layout_union_rect(&state.ink, ink)) {
      r = gfnt_error_set(error, GFNT_ERR_INVALID, 0, state.lines[li].start,
          GFNT_GLYPH_NONE, "layout ink bounds exceed the supported coordinate range");
      goto done;
    }
  }
  if (constraint) {
    width = 0;
    for (size_t li = 0; li < state.line_count; li++) {
      int64_t end = (int64_t)state.band_starts[li] + state.band_widths[li];
      if (end > width) width = (GFNT_F26Dot6)end;
    }
  }
  layout_state_finish(&state, out, p, width, vertical, state.y, state.widest,
      state.line_count, state.any_truncated, state.ink);
  return GFNT_OK;
done:
  layout_state_free(&state);
  return r;
}

GFNT_Result gfnt_paragraph_layout_flow(const GFNT_Paragraph * p,
    const GFNT_LineConstraint * constraint, GFNT_Layout * out, GFNT_Error * error) {
  if (!constraint) return GFNT_ERR_INVALID;
  return paragraph_layout_internal(p, GFNT_LAYOUT_UNBOUNDED, constraint, out, error);
}

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

static bool layout_union_rect(GFNT_Rect * bounds, const GFNT_Rect * rect) {
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
    GFNT_F26Dot6 inline_shift = (GFNT_F26Dot6)((int64_t)ci
        * ((int64_t)options->inline_size + options->inline_gap));
    if (vertical) {
      shifted_y = inline_shift;
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
              - options->inline_size - inline_shift
          : inline_shift;
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
    free_line_annotation_tree(allocator,
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
      if (!layout_union_rect(&full.ink, &ink)) {
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

static void flow_include_x(long double x, long double * lo, long double * hi, bool * found) {
  if (!*found || x < *lo) *lo = x;
  if (!*found || x > *hi) *hi = x;
  *found = true;
}

/** Bounds of a polygon clipped to a horizontal slab. */
static bool flow_polygon_slab(const GFNT_FlowPoint * points, size_t count,
    long double y0, long double y1, long double * x0, long double * x1,
    long double * bottom) {
  bool found = false;
  long double lo = 0, hi = 0, min_y = points[0].y, max_y = points[0].y;
  for (size_t i = 1; i < count; i++) {
    if (points[i].y < min_y) min_y = points[i].y;
    if (points[i].y > max_y) max_y = points[i].y;
  }
  if (y1 <= min_y || y0 >= max_y) return false;
  for (size_t i = 0; i < count; i++) {
    const GFNT_FlowPoint a = points[i], b = points[(i + 1) % count];
    const long double ay = a.y, by = b.y;
    if (ay > max_y) max_y = ay;
    if (ay >= y0 && ay <= y1) flow_include_x(a.x, &lo, &hi, &found);
    if (ay != by) {
      const long double low = ay < by ? ay : by;
      const long double high = ay > by ? ay : by;
      const long double scans[] = {y0, y1};
      for (size_t k = 0; k < 2; k++) {
        const long double y = scans[k];
        if (y >= low && y <= high) {
          const long double x = a.x + (y - ay) * ((long double)b.x - a.x) / (by - ay);
          flow_include_x(x, &lo, &hi, &found);
        }
      }
    }
  }
  if (!found) return false;
  *x0 = lo; *x1 = hi; *bottom = max_y;
  return true;
}

/** Bounds of one exclusion over the line's cross-axis interval. */
static bool flow_exclusion_slab(const GFNT_Exclusion * item, GFNT_F26Dot6 top,
    GFNT_F26Dot6 height, long double * x0, long double * x1, int64_t * bottom) {
  const long double margin = item->margin;
  const long double y0 = (long double)top - margin;
  const long double y1 = (long double)top + height + margin;
  long double lo = 0, hi = 0, lower = 0;
  switch (item->shape) {
    case GFNT_EXCLUSION_RECTANGLE: {
      const GFNT_Rect r = item->geometry.rectangle;
      const long double upper = (long double)r.y - margin;
      lower = (long double)r.y + r.height + margin;
      if (y1 <= upper || y0 >= lower) return false;
      lo = (long double)r.x - margin;
      hi = (long double)r.x + r.width + margin;
      break;
    }
    case GFNT_EXCLUSION_CIRCLE: {
      const GFNT_FlowPoint center = item->geometry.circle.center;
      const long double radius = (long double)item->geometry.circle.radius + margin;
      const long double top_edge = (long double)center.y - radius;
      lower = (long double)center.y + radius;
      if (y1 <= top_edge || y0 >= lower) return false;
      long double dy = 0;
      if ((long double)center.y < y0) dy = y0 - center.y;
      else if ((long double)center.y > y1) dy = center.y - y1;
      const long double reach = sqrtl(radius * radius - dy * dy);
      lo = (long double)center.x - reach;
      hi = (long double)center.x + reach;
      break;
    }
    case GFNT_EXCLUSION_POLYGON:
      if (!flow_polygon_slab(item->geometry.polygon.points, item->geometry.polygon.count,
              y0, y1, &lo, &hi, &lower)) return false;
      lo -= margin; hi += margin; lower += margin;
      break;
    default:
      return false;
  }
  lo = floorl(lo); hi = ceill(hi);
  if (lo < INT32_MIN || hi > INT32_MAX || lower < INT32_MIN || lower > INT32_MAX) return false;
  *x0 = lo; *x1 = hi; *bottom = (int64_t)lower;
  return true;
}

GFNT_Result gfnt_exclusions_band(void * ctx, GFNT_F26Dot6 top,
    GFNT_F26Dot6 height, GFNT_Band * out) {
  const GFNT_Exclusions * exclusions = ctx;
  int64_t start, end, next_top;
  bool blocked = false;
  if (!exclusions || !out || top < 0 || height < 0 || exclusions->width < 0
      || (exclusions->count && !exclusions->items)) return GFNT_ERR_INVALID;
  start = exclusions->start;
  end = start + (int64_t)exclusions->width;
  next_top = top;
  if (start < INT32_MIN || start > INT32_MAX || end < INT32_MIN || end > INT32_MAX) {
    return GFNT_ERR_INVALID;
  }
  for (size_t i = 0; i < exclusions->count; i++) {
    const GFNT_Exclusion * item = &exclusions->items[i];
    long double left, right;
    int64_t bottom;
    bool affects = false;
    if (item->margin < 0 || item->side < GFNT_EXCLUSION_TEXT_RIGHT
        || item->side > GFNT_EXCLUSION_TEXT_LEFT || item->shape < GFNT_EXCLUSION_RECTANGLE
        || item->shape > GFNT_EXCLUSION_POLYGON) return GFNT_ERR_INVALID;
    if (item->shape == GFNT_EXCLUSION_RECTANGLE) {
      const GFNT_Rect r = item->geometry.rectangle;
      const int64_t right_edge = (int64_t)r.x + r.width + item->margin;
      const int64_t bottom_edge = (int64_t)r.y + r.height + item->margin;
      if (r.width < 0 || r.height < 0 || (int64_t)r.x - item->margin < INT32_MIN
          || right_edge > INT32_MAX || (int64_t)r.y - item->margin < INT32_MIN
          || bottom_edge > INT32_MAX) return GFNT_ERR_INVALID;
    }
    if (item->shape == GFNT_EXCLUSION_CIRCLE) {
      const int64_t radius = (int64_t)item->geometry.circle.radius + item->margin;
      const GFNT_FlowPoint center = item->geometry.circle.center;
      if (item->geometry.circle.radius < 0 || (int64_t)center.x - radius < INT32_MIN
          || (int64_t)center.x + radius > INT32_MAX || (int64_t)center.y - radius < INT32_MIN
          || (int64_t)center.y + radius > INT32_MAX) return GFNT_ERR_INVALID;
    }
    if (item->shape == GFNT_EXCLUSION_POLYGON
        && (!item->geometry.polygon.points || item->geometry.polygon.count < 3)) {
      return GFNT_ERR_INVALID;
    }
    if (item->shape == GFNT_EXCLUSION_POLYGON) {
      for (size_t j = 0; j < item->geometry.polygon.count; j++) {
        const GFNT_FlowPoint point = item->geometry.polygon.points[j];
        if ((int64_t)point.x - item->margin < INT32_MIN
            || (int64_t)point.x + item->margin > INT32_MAX
            || (int64_t)point.y - item->margin < INT32_MIN
            || (int64_t)point.y + item->margin > INT32_MAX) return GFNT_ERR_INVALID;
      }
    }
    if (!flow_exclusion_slab(item, top, height, &left, &right, &bottom)) continue;
    if (item->side == GFNT_EXCLUSION_TEXT_RIGHT) {
      if (right > start) {
        start = (int64_t)right;
        affects = true;
      }
    }
    else if (left < end) {
      end = (int64_t)left;
      affects = true;
    }
    if (affects && bottom > top) {
      if (!blocked || bottom < next_top) next_top = bottom;
      blocked = true;
    }
  }
  if (start < INT32_MIN || start > INT32_MAX || end < INT32_MIN || end > INT32_MAX
      || end - start > INT32_MAX || next_top > INT32_MAX) return GFNT_ERR_INVALID;
  out->start = (GFNT_F26Dot6)start;
  out->width = end > start ? (GFNT_F26Dot6)(end - start) : 0;
  out->next_top = blocked ? (GFNT_F26Dot6)next_top : top;
  return GFNT_OK;
}
