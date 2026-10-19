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
 * Annotation tracks on a laid-out line: fragments, ruby, emphasis marks.
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

/* ------------------------------------------------------------------ layout */

void gfnt_lay_free_line_annotation_tree(const GFNT_Allocator * allocator,
    GFNT_LineAnnotation * annotations, size_t count) {
  if (!annotations) return;
  for (size_t ai = 0; ai < count; ai++) {
    GFNT_LineRun * runs = (GFNT_LineRun *)annotations[ai].runs;
    for (size_t ri = 0; ri < annotations[ai].run_count; ri++) {
      allocator->free_fn(allocator->ctx, (void *)runs[ri].glyphs);
    }
    allocator->free_fn(allocator->ctx, runs);
    gfnt_lay_free_line_annotation_tree(allocator,
        (GFNT_LineAnnotation *)annotations[ai].children, annotations[ai].child_count);
  }
  allocator->free_fn(allocator->ctx, annotations);
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
  gfnt_lay_free_line_annotation_tree(allocator, (GFNT_LineAnnotation *)annotation->children,
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
    if (!gfnt_lay_annotation_byte_cp(paragraph, start, &cp_start)
        || !gfnt_lay_annotation_byte_cp(paragraph, end, &cp_end)) {
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

/** The cross-axis room the same-side ruby tracks on one base take, and the part before this one. */
static GFNT_Result annotation_same_side_extent(const GFNT_Paragraph * p, const PAnnotation * source,
    size_t index, size_t base_cp_start, size_t base_cp_end, int64_t * total, int64_t * before,
    GFNT_Error * error) {
  int64_t same_side_total = 0, same_side_before = 0;
  GFNT_Result r;

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
        return r;
      }
      if (!fragment_present) continue;
      const int64_t extent = fragment_extent;
      if (other->descriptor.position != source->descriptor.position) continue;
      if (extent < 0 || same_side_total > INT32_MAX - extent) {
        return GFNT_ERR_LIMIT;
      }
      same_side_total += extent;
      if (ai < index) same_side_before += extent;
    }
  }
  *total = same_side_total;
  *before = same_side_before;
  return GFNT_OK;
}

/** Where an over-wide ruby starts once it may hang over a plain neighbor, and no further than the line. */
static GFNT_F26Dot6 annotation_overhang_start(const GFNT_Paragraph * p, const PAnnotation * source,
    const GFNT_Line * base_line, const GFNT_Line * source_line, bool vertical, GFNT_F26Dot6 lo,
    GFNT_F26Dot6 hi, GFNT_F26Dot6 target, GFNT_F26Dot6 base_lo) {
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
  return lo;
}

/** The shifts that put the annotation's line where its base is: along the line and across it. */
static GFNT_Result annotation_shifts(const GFNT_Paragraph * p, const PAnnotation * source,
    const GFNT_Line * base_line, const GFNT_Line * source_line, bool vertical, GFNT_F26Dot6 lo,
    GFNT_F26Dot6 base_cross_half, int64_t same_side_total, int64_t same_side_before,
    GFNT_F26Dot6 * out_along, GFNT_F26Dot6 * out_across) {
  GFNT_F26Dot6 shift_along, shift_across;

  if (vertical) {
    const int64_t along = (int64_t)lo - source_line->offset;
    if (along < INT32_MIN || along > INT32_MAX) {
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
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
    else if (source->descriptor.kind != GFNT_ANNOTATION_RUBY) {
      const int64_t value = source->descriptor.position == GFNT_ANNOTATION_UNDER
          ? (int64_t)base_line->baseline - base_line->descent + source_line->ascent
          : (int64_t)base_line->baseline + base_line->ascent - source->ascent;
      if (value < INT32_MIN || value > INT32_MAX) {
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
    else if (place_left) {
      const int64_t extent = (int64_t)source_line->ascent + source_line->descent;
      const int64_t value = (int64_t)base_line->baseline - base_cross_half
          - same_side_before - extent;
      if (value < INT32_MIN || value > INT32_MAX) {
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
    else {
      const int64_t value = (int64_t)base_line->baseline + base_cross_half
          + same_side_before;
      if (value < INT32_MIN || value > INT32_MAX) {
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
  }
  else {
    const int64_t along = (int64_t)lo - source_line->offset;
    if (along < INT32_MIN || along > INT32_MAX) {
      return GFNT_ERR_LIMIT;
    }
    shift_along = (GFNT_F26Dot6)along;
    if (source->descriptor.kind != GFNT_ANNOTATION_RUBY) {
      const int64_t value = source->descriptor.position == GFNT_ANNOTATION_UNDER
          ? (int64_t)base_line->baseline + base_line->descent - source_line->descent
          : (int64_t)base_line->baseline - base_line->ascent + source_line->ascent;
      if (value < INT32_MIN || value > INT32_MAX) {
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
    else if (source->descriptor.position == GFNT_ANNOTATION_UNDER) {
      const int64_t value = (int64_t)base_line->baseline + base_line->descent
          - same_side_total + same_side_before + source_line->ascent;
      if (value < INT32_MIN || value > INT32_MAX) {
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
    else {
      const int64_t extent = (int64_t)source_line->ascent + source_line->descent;
      const int64_t value = (int64_t)base_line->baseline - base_line->ascent
          + same_side_total - same_side_before - extent + source_line->ascent;
      if (value < INT32_MIN || value > INT32_MAX) {
        return GFNT_ERR_LIMIT;
      }
      shift_across = (GFNT_F26Dot6)value;
    }
  }
  *out_along = shift_along;
  *out_across = shift_across;
  return GFNT_OK;
}

/** Copy the shaped annotation's runs and nested tracks into @p out, moved by the shifts. */
static GFNT_Result annotation_emit(const GFNT_Allocator * allocator, const PAnnotation * source,
    size_t index, size_t base_start, size_t base_end, const GFNT_Line * source_line, bool vertical,
    GFNT_F26Dot6 shift_along, GFNT_F26Dot6 shift_across, GFNT_F26Dot6 dx, GFNT_F26Dot6 dy,
    GFNT_LineAnnotation * out) {
  out->base_start = base_start;
  out->base_end = base_end;
  out->kind = source->descriptor.kind;
  out->position = source->descriptor.position;
  out->annotation_index = index;
  out->run_count = source_line->run_count;
  GFNT_LineRun * runs = allocator->calloc_fn(allocator->ctx,
      out->run_count ? out->run_count : 1, sizeof *runs);
  if (!runs) {
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
        gfnt_lay_free_line_annotation_tree(allocator, children, out->child_count);
        out->children = NULL;
        out->child_count = 0;
        return child_result;
      }
      out->child_count++;
    }
  }
  return GFNT_OK;
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
  if (r != GFNT_OK) {
    goto done;
  }
  if (!shaped.line_count) {
    r = GFNT_OK;
    goto done;
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
  r = annotation_same_side_extent(p, source, index, base_cp_start, base_cp_end,
      &same_side_total, &same_side_before, error);
  if (r != GFNT_OK) {
    goto done;
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
  lo = annotation_overhang_start(p, source, base_line, source_line, vertical, lo, hi, target,
      base_lo);
  r = annotation_shifts(p, source, base_line, source_line, vertical, lo, base_cross_half,
      same_side_total, same_side_before, &shift_along, &shift_across);
  if (r != GFNT_OK) {
    goto done;
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
      r = GFNT_ERR_LIMIT;
      goto done;
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
      r = GFNT_ERR_LIMIT;
      goto done;
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
        r = GFNT_ERR_LIMIT;
        goto done;
      }
      base_line->ink.x = (GFNT_F26Dot6)left;
      base_line->ink.y = (GFNT_F26Dot6)top;
      base_line->ink.width = (GFNT_F26Dot6)(right - left);
      base_line->ink.height = (GFNT_F26Dot6)(bottom - top);
    }
  }
  r = annotation_emit(allocator, source, index, base_start, base_end, source_line, vertical,
      shift_along, shift_across, dx, dy, out);
done:
  gfnt_layout_free(&shaped);
  gfnt_paragraph_free(fragment_paragraph);
  return r;
}

GFNT_Result gfnt_lay_shape_default_emphasis(const GFNT_Paragraph * p, const PAnnotation * source,
    uint32_t cp, PAnnotation * shaped, GFNT_Error * error) {
  static const char dot[] = "•";
  static const char sesame[] = "﹅";
  const bool cjk_vertical = p->vertical && gfnt_lay_is_ideographic(cp);
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

GFNT_Result gfnt_lay_add_line_annotations(const GFNT_Paragraph * p, GFNT_Line * lines,
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
              || unit->space || unit->tab || gfnt_lay_is_newline(p->cps[unit->cp_start])) continue;
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
          if (unit->space || unit->tab || gfnt_lay_is_newline(p->cps[unit->cp_start])
              || start < line->start || start >= line->end) continue;
          const bool use_default = annotation->descriptor.mark_kind == GFNT_EMPHASIS_MARK_DEFAULT;
          const bool cjk_vertical = p->vertical && gfnt_lay_is_ideographic(p->cps[unit->cp_start]);
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
