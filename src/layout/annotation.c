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
 * Ruby and emphasis annotations: validation, shaping and the tree kept with the paragraph.
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
      && a->inline_object == b->inline_object
      && a->text_transform == b->text_transform
      && a->case_tailoring == b->case_tailoring;
}

static bool annotation_merge_compatible(const GFNT_Annotation * a, const GFNT_Annotation * b) {
  return a->merge && b->merge && a->kind == GFNT_ANNOTATION_RUBY
      && b->kind == GFNT_ANNOTATION_RUBY && a->base_end == b->base_start
      && a->position == b->position && a->align == b->align
      && a->group_break == b->group_break && a->overhang == b->overhang
      && !a->child_count && !b->child_count
      && annotation_style_equal(&a->style, &b->style);
}

bool gfnt_lay_annotation_tree_valid(const GFNT_Annotation * items, size_t count,
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
    if (!gfnt_lay_annotation_tree_valid(a->children, a->child_count,
            a->text_length, 0, a->text_length, depth + 1)) return false;
  }
  return true;
}

bool gfnt_lay_annotation_byte_cp(const GFNT_Paragraph * p, size_t byte, size_t * cp) {
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



GFNT_Result gfnt_lay_create_annotations(GFNT_Paragraph * p, const GFNT_Annotation * items,
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
    if (!gfnt_lay_annotation_byte_cp(p, source->base_start, &annotation->cp_start)
        || !gfnt_lay_annotation_byte_cp(p, source->base_end, &annotation->cp_end)) {
      return gfnt_error_set(error, GFNT_ERR_INVALID, 0, source->base_start, GFNT_GLYPH_NONE,
          "an annotation range ends inside a UTF-8 character");
    }
    if (source->kind == GFNT_ANNOTATION_EMPHASIS
        && source->mark_kind != GFNT_EMPHASIS_MARK_STRING) {
      static const char * const marks[] = {"•", "•", "◦", "◎", "▲", "﹅", ""};
      size_t base = annotation->cp_start;
      uint32_t cp = base < p->length ? p->cps[base] : 0;
      bool cjk = gfnt_lay_is_ideographic(cp);
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
        r = gfnt_lay_shape_default_emphasis(p, annotation, alternate_cp, &alternate, error);
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

/** Choose one line end without mutating paragraph state. */
/** The amount of a ruby group's reserved advance assigned to one base unit. */
GFNT_F26Dot6 gfnt_lay_ruby_unit_share(const PAnnotation * annotation, size_t unit,
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
GFNT_F26Dot6 gfnt_lay_ruby_right_refund(const GFNT_Paragraph * p, size_t line_start,
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
      shares += gfnt_lay_ruby_unit_share(a, u, a->break_extra);
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
